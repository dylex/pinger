#!/usr/bin/perl -w
use strict;
use POSIX qw(strftime);

local $, = " ";
local $\ = "\n";

sub tfmt($) { scalar(localtime($_[0])) }
sub msg($@) { 
	my $t = shift;
	print tfmt($t), @_;
}

my $N = 0;
my $M = 0;
my $S = 0;
my $n = 0;
my $s = 0;
my ($t, $p);
while (<>) {
	chomp;
	($t, $p) = split;
	if ($p eq 'X') {
		if ($n > 0) {
			msg $t, $n, $s/$n, "DOWN";
			$n = 0;
			$s = 0;
		}
		$n --;
		$M ++;
	} else {
		if ($n < 0) {
			msg $t, -$n, "UP";
			$n = 0;
		}
		$n ++;
		$N ++;
		$s += $p;
		$S += $p;
	}
}
msg $t, $n, $s/$n if $n;
printf "%d/%d %g%% %gms\n", $M, ($N+$M), 100*$M/($N+$M), $S/$N;
