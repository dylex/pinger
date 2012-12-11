{-# LANGUAGE ScopedTypeVariables, MultiParamTypeClasses, FunctionalDependencies, FlexibleInstances, FlexibleContexts #-}
module Main (main) where

import Control.Applicative
import Control.Arrow (first, second)
import Control.Monad
import Data.Bits
import Data.Fixed (Micro)
import Data.List
import qualified Data.Map as Map
import Data.Time.Clock.POSIX (posixSecondsToUTCTime)
import Data.Time.Format (formatTime)
import Data.Time.LocalTime (TimeZone, getCurrentTimeZone, utcToLocalTime)
import Data.Ratio
import Data.Word
import Foreign.Ptr
import Foreign.Storable
import GHC.IO (unsafeDupableInterleaveIO, unsafeDupablePerformIO)
import Network.Socket (HostAddress, inet_ntoa)
import Numeric
import System.Console.GetOpt
import System.Environment
import System.Exit
import System.IO
import System.IO.MMap
import System.Locale (defaultTimeLocale)

type Datum = Word32
type Time = Micro

data Threshold 
  = ThreshDead
  | ThreshTime Time
  | ThreshSD Float
  | ThreshPct Float

instance Read Threshold where
  readsPrec d = map (uncurry rt) . readsPrec d where
    rt t ('m':'s':r) | t > 0 = (ThreshTime $ realToFrac t/1000, r)
    rt s ('s':'d':r) = (ThreshSD s, r)
    rt t ('s':r) | t > 0 = (ThreshTime $ realToFrac t, r)
    rt s ('z':r) = (ThreshSD s, r)
    rt p ('%':r) | p > 0 && p < 100 = (ThreshPct (p/100), r)
    rt t r = (ThreshTime $ realToFrac t, r)

data Options = Options
  { optDump :: Bool
  , optRun :: Int
  , optThresh :: Threshold
  }

defOptions :: Options
defOptions = Options
  { optDump = False
  , optRun = 0
  , optThresh = ThreshDead
  }

options :: [OptDescr (Options -> Options)]
options =
  [ Option "d" ["dump"] (NoArg (\o -> o{ optDump = True })) "dump all ping results"
  , Option "r" ["run"] (OptArg (\x o -> o{ optRun = maybe 1 read x }) "LENGTH") "show (runs of LENGTH) over threshold"
  , Option "t" ["thresh"] (ReqArg (\x o -> o{ optThresh = read x }) "TIME") "use run threshold >= TIME (s,ms,sd,%)"
  ]

readArray :: Storable a => Int -> Ptr a -> IO [a]
readArray 0 _ = return []
readArray n p = unsafeDupableInterleaveIO $ do
  x <- peek p
  let z = sizeOf x
  (x :) <$> readArray (n - z) (p `plusPtr` z)

hostMax :: Datum
hostMax = 255

deltaBit :: Int
deltaBit = bitSize (0 :: Datum) - 1

datumToTime :: Datum -> Time
datumToTime t = fromRational (fromIntegral t % 1000000)

data Response 
  = Live !Time
  | Dead
  deriving (Show, Eq, Ord)

datumToResponse :: Datum -> Response
datumToResponse p
  | testBit p deltaBit = Dead
  | otherwise = Live $ datumToTime p

type RawData = Ptr Datum

type Chunk = ([HostAddress], [(Time, [Response])])

parseData :: [Datum] -> [Chunk]
parseData [] = []
parseData (ii:ir)
  | ii > hostMax = error $ "invalid file format (got " ++ show ii ++ ")"
  | otherwise = gc (error "no start time") ii ir where
  gc t n r = uncurry ((:) . (,) h) $ gd t n' hr where
    n' = fromIntegral n
    (h,hr) = splitAt n' r
  gd t n (i:r)
    | i <= hostMax = ([], gc t i r)
    | testBit i deltaBit = gr (t + datumToTime (clearBit i deltaBit)) n r
  gd _ n (s:t:r) = gr (fromIntegral s + datumToTime t) n r
  gd _ _ _ = ([], [])
  gr t n r = first ((t,map datumToResponse p) :) $ gd t n pr where (p,pr) = splitAt n r

type Ping = (Time, Response)
type Hosts = Map.Map HostAddress [Ping]

hostsData :: [Chunk] -> Hosts
hostsData [] = Map.empty
hostsData ((al, pl) : r) = foldr (uncurry $ Map.insertWith (++)) (hostsData r) $ zip al $ transpose $ map (uncurry $ map . (,)) pl

splitResponses :: [Response] -> (Int, [Time])
splitResponses (Live t:l) = second (t :) $ splitResponses l
splitResponses (Dead:l) = first succ $ splitResponses l
splitResponses [] = (0, [])

runs :: Int -> (a -> Bool) -> [a] -> [[a]]
runs n f = rf where
  rf [] = []
  rf l
    | null y = rf (tail r)
    | null r || length y >= n = y : rf r
    | otherwise = rf r
    where (y,r) = span f l

data Stats = Stats
  { countTotal, countLive, countDead :: Int
  , statDead :: Ratio Int
  , statStart, statEnd :: Time
  , statMean, statSD :: Double
  , statMin, statMax :: Response
  , statMedian :: Response
  , threshold :: Threshold -> Response
  }

stats :: [(Time,Response)] -> Stats
stats [] = Stats 0 0 0 1 0 0 0 0 Dead Dead Dead (const Dead)
stats d = Stats
  { countLive = cl
  , countDead = cd
  , countTotal = ct
  , statDead = cd % ct
  , statStart = fst (head d)
  , statEnd = fst (last d)
  , statMean = m
  , statSD = sd
  , statMin = if null dl then Dead else Live $ minimum dl
  , statMax = if null dl then Dead else Live $ maximum dl
  , statMedian = nth (ct `div` 2)
  , threshold = th
  } where
  dr = map snd d
  (cd,dl) = splitResponses dr
  cl = length dl
  ct = cd + cl
  m = realToFrac $ sum dl / fromIntegral cl
  sd = sqrt $ sum (map (join (*) . realToFrac) dl) / fromIntegral cl - m*m
  ds = sortBy (flip compare) dl
  nth i
    | i < cd = Dead
    | otherwise = Live $ ds !! (i - cd)
  th ThreshDead = Dead
  th (ThreshTime tt) = Live tt
  th (ThreshSD ts) = Live $ realToFrac $ m + (realToFrac ts) * sd
  th (ThreshPct tp) = nth $ floor $ tp * fromIntegral ct

ss :: String -> ShowS
ss = showString

sc :: Char -> ShowS
sc = showChar

sscaled :: Real a => Double -> a -> ShowS
sscaled s = showFFloat (Just 3) . (s*) . realToFrac

ms :: Real a => a -> ShowS
ms = sscaled 1000

mms :: Response -> ShowS
mms Dead = sc 'X'
mms (Live t) = ms t

timeZone :: TimeZone
timeZone = unsafeDupablePerformIO getCurrentTimeZone

showsTime :: Time -> ShowS
showsTime = ss . formatTime defaultTimeLocale "%c" . utcToLocalTime timeZone . posixSecondsToUTCTime . realToFrac

showStats :: Stats -> String
showStats Stats{ countTotal = 0 } = "no data"
showStats st =
  showsTime (statStart st) $ ss "--" $ showsTime (statEnd st) $ sc ' ' $
  sscaled 100 (statDead st) $ ss "% " $
  shows (countTotal st) $ sc ' ' $
  if countLive st == 0 then "" else
  mms (statMedian st) $ ss "ms " $
  ms (statMean st) $ sc '\xb1' $ ms (statSD st) $
  ss "ms [" $ mms (statMin st) $ sc ',' $ mms (statMax st) $ "]"

main :: IO ()
main = do
  prog <- getProgName
  args <- getArgs
  (file, opts) <- case getOpt Permute options args of
    (o, [f], []) -> return (f, foldl' (flip ($)) defOptions o)
    (_, _, errs) -> do
      mapM_ (hPutStrLn stderr) errs
      hPutStrLn stderr $ usageInfo ("Usage: "  ++ prog ++ " FILE") options
      exitFailure
  (dptr, dptrz, 0, dz) <- mmapFilePtr file ReadOnly Nothing
  da <- readArray dz (dptr :: RawData)
  let hd = hostsData $ parseData da
  forM_ (Map.toList hd) $ \(a, d) -> do
    hn <- inet_ntoa a
    let st = stats d
    putStrLn $ ss hn $ ss ": " $ showStats st
    when (optRun opts > 0) $ do
      let th = threshold st (optThresh opts)
          rs = runs (optRun opts) ((>= th) . snd) d
      forM_ rs $ putStrLn . sc '\t' . showStats . stats
    when (optDump opts) $ forM_ d $ \(t,r) -> do
      putStrLn $ sc '\t' $ showsTime t $ sc '\t' $ mms r $ ""
  munmapFilePtr dptr dptrz
