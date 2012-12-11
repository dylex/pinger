#!/usr/bin/perl -w
use strict;
use Getopt::Std;
use POSIX qw(fmod);
use Net::Ping;
use Time::HiRes qw(time sleep);

our $opt_i = 60;
our $opt_I = 60;
getopt('i:I:');

die "Usage: $0 [-i INTERVAL] HOST ...\n" unless @ARGV;

my $p = Net::Ping->new("icmp", 5);
$p->hires(1);

local $, = " ";
local $\ = "\n";
local $| = 1;

my $b = 0;
while (1)
{
	my $st = $b ? $opt_I : $opt_i;
	sleep($st - fmod(time, $st));
	my $t = sprintf("%.3f", time);
	my @p;
	$b = 0;
	for my $h (@ARGV)
	{
		my ($r, $dt) = $p->ping($h);
		push @p, $r ? sprintf("%7.2f", 1000*$dt) : 'X';
		$b ++ unless $r;
	}
	print $t, @p;
}
