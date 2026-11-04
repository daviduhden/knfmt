#!/usr/bin/env perl
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
# SPDX-License-Identifier: ISC

use strict;
use warnings;

my $mode = shift @ARGV // '';
die "usage: $0 output|signal\n" if @ARGV;
if ( $mode eq 'output' ) {
    binmode STDOUT, ':raw' or die "cannot set stdout mode: $!";
    binmode STDERR, ':raw' or die "cannot set stderr mode: $!";
    print STDOUT 'o' x 100000 or die "cannot write stdout: $!";
    print STDERR 'e' x 100000 or die "cannot write stderr: $!";
    close STDOUT              or die "cannot close stdout: $!";
    close STDERR              or die "cannot close stderr: $!";
    exit 7;
}
if ( $mode eq 'signal' ) {
    kill 'TERM', $$ or die "cannot signal child: $!";
    die "termination signal was not delivered\n";
}
die "usage: $0 output|signal\n";
