#!/usr/bin/env perl
#
# Copyright (c) 2026 David Uhden Collado <daviduhden@gmail.com>
# SPDX-License-Identifier: ISC
#
# Validate the shared Agent Knowledge Validation contract.
#
# The canonical Agent Knowledge Validation implementation.
# It uses only core modules and Git. The command line, rule identifiers,
# output formats, and exit codes are intentionally compatible.
#
# See the LICENSE file at the top of the project tree for copyright
# and license details.

use 5.020;
use strict;
use warnings;

use Cwd            qw(abs_path getcwd realpath);
use Encode         qw(decode encode FB_CROAK FB_DEFAULT);
use File::Basename qw(basename dirname);
use File::Spec;
use Errno qw(EINTR);
use IO::Select;
use IPC::Open3   qw(open3);
use JSON::PP     ();
use Symbol       qw(gensym);
use Scalar::Util ();

my $CONTRACT_VERSION = '1.0.0';
my $SCHEMA_VERSION   = '1.0';
my $CONFIG_NAME      = '.agent-knowledge-validator.toml';

my %RULE_NAMES = (
    AK001 => 'Agent knowledge synchronization',
    AK002 => 'Agent and skill structure',
    AK003 => 'Internal references',
    AK004 => 'Knowledge update relevance',
    AK005 => 'Validator configuration and execution',
    AK006 => 'Duplicate or conflicting instructions',
);

my %INSTRUCTION_NAMES = map { $_ => 1 } (
    'AGENTS.md',    'AGENT.md',
    'SKILL.md',     'CLAUDE.md',
    'GEMINI.md',    'copilot-instructions.md',
    '.cursorrules', '.windsurfrules',
);

my @INSTRUCTION_SUFFIXES = ( '.agent.md', '.instructions.md', '.mdc' );

my @INFRASTRUCTURE_PATTERNS = (
    'scripts/validate-agent-knowledge.pl',
    '**/scripts/validate-agent-knowledge.pl',
    # Historical paths are data: ranges containing their removal stay exempt.
    'scripts/validate-agent-knowledge.py',
    '**/scripts/validate-agent-knowledge.py',
    'tests/agent_knowledge_validator/**',
    '**/tests/agent_knowledge_validator/**',
    '.github/workflows/agent-knowledge-validation.yml',
    '**/agent-knowledge-validation.yml',
    '**/agent-knowledge-validation.yaml',
);

my @DEFAULT_EXCLUDED_PATHS = ( "__pycache__/**", "**/__pycache__/**" );

#
# Git execution. No shell is involved; every argument is passed as a list.
#

sub capture_command {
    my (@command) = map { utf8::is_utf8($_) ? encode( 'UTF-8', $_ ) : $_ } @_;
    my $stderr_handle = gensym;
    my ( $input, $output );
    my $pid = open3( $input, $output, $stderr_handle, @command );
    close $input;
    binmode $output;
    binmode $stderr_handle;
    my $stdout_fd = fileno($output);
    my $selector  = IO::Select->new( $output, $stderr_handle );
    my ( $stdout, $stderr ) = ( '', '' );
    my $error;
    eval {
        while ( $selector->count ) {
            my @ready = $selector->can_read();
            next if !@ready && $! == EINTR;
            die "cannot wait for child output: $!\n" unless @ready;
            for my $handle (@ready) {
                my $count = sysread( $handle, my $chunk, 8192 );
                if ( !defined $count ) {
                    next if $! == EINTR;
                    die "cannot read child output: $!\n";
                }
                if ( $count == 0 ) {
                    $selector->remove($handle);
                    close $handle;
                    next;
                }
                if ( fileno($handle) == $stdout_fd ) {
                    $stdout .= $chunk;
                }
                else {
                    $stderr .= $chunk;
                }
            }
        }
    };
    $error = $@;
    if ($error) {
        close $_ for $selector->handles;
        kill 'TERM', $pid;
    }
    my $waited;
    do { $waited = waitpid( $pid, 0 ); } while $waited == -1 && $! == EINTR;
    die "cannot wait for child: $!\n" if $waited == -1;
    my $status = $?;
    die $error if $error;
    return ( $stdout, $stderr,
        $status & 127 ? 128 + ( $status & 127 ) : $status >> 8 );
}

sub trim_text {
    my ($value) = @_;
    return '' unless defined $value;
    $value =~ s/^\s+//;
    $value =~ s/\s+$//;
    return $value;
}

sub split_text_lines {
    my ($value) = @_;
    return () unless defined $value && $value ne '';
    return split /\r\n|[\n\r\v\f\x1c\x1d\x1e\x85\x{2028}\x{2029}]/, $value;
}

sub run_git_optional {
    my ( $root, @arguments ) = @_;
    my ( $stdout, $stderr, $status ) =
      capture_command( 'git', '-C', $root, @arguments );
    $stdout = decode( 'UTF-8', $stdout, FB_DEFAULT );
    $stderr = decode( 'UTF-8', $stderr, FB_DEFAULT );
    s/\r\n?/\n/g for $stdout, $stderr;
    return ( $stdout, $stderr, $status );
}

sub run_git {
    my ( $root, @arguments ) = @_;
    my ( $stdout, $stderr, $status ) = run_git_optional( $root, @arguments );
    if ($status) {
        my $detail = trim_text($stderr);
        $detail = 'git ' . join( ' ', @arguments ) . ' failed'
          if $detail eq '';
        die $detail . "\n";
    }
    return $stdout;
}

sub filesystem_text {
    my ($value) = @_;
    return $value if !defined $value || utf8::is_utf8($value);
    return decode( 'UTF-8', $value, FB_DEFAULT );
}

sub find_repository_root {
    my ($cwd)    = @_;
    my $output   = run_git( $cwd, 'rev-parse', '--show-toplevel' );
    my $root     = trim_text($output);
    my $resolved = realpath($root);
    $resolved = abs_path($root)            unless defined $resolved;
    $resolved = File::Spec->rel2abs($root) unless defined $resolved;
    return filesystem_text($resolved);
}

sub resolve_commit {
    my ( $root, $revision ) = @_;
    my ( $stdout, $stderr, $status ) =
      run_git_optional( $root, 'rev-parse', '--verify',
        $revision . '^{commit}' );
    if ($status) {
        die "revision does not resolve to a commit: $revision\n";
    }
    return trim_text($stdout);
}

#
#
# Schema-oriented TOML reader. All TOML 1.0 spellings of values accepted by
# this configuration schema are supported, including multiline strings,
# dotted/quoted keys, inline tables, arrays and integer bases. Other TOML
# scalar types and arrays of tables cannot satisfy any schema setting and
# are rejected. This is not a general-purpose TOML implementation.
#

{

    package AKV::Int;
    sub value { return ${ $_[0] }; }

    package AKV::Bool;
    sub value { return ${ $_[0] }; }

    package main;
}

sub toml_integer {
    my ($value) = @_;
    return bless \$value, 'AKV::Int';
}

sub toml_boolean {
    my ($value) = @_;
    my $stored = $value ? 1 : 0;
    return bless \$stored, 'AKV::Bool';
}

sub toml_die {
    my ( $state, $message ) = @_;
    my $line = 1 + ( substr( $state->{text}, 0, $state->{pos} ) =~ tr/\n/\n/ );
    die "line $line: $message\n";
}

sub toml_peek {
    my ( $state, $count ) = @_;
    return substr( $state->{text}, $state->{pos}, $count || 1 );
}

sub toml_comment {
    my ($state) = @_;
    my $end = index( $state->{text}, "\n", $state->{pos} );
    $end = $state->{length} if $end < 0;
    my $comment = substr( $state->{text}, $state->{pos}, $end - $state->{pos} );
    toml_die( $state, 'invalid control character in comment' )
      if $comment =~ /[\x00-\x08\x0b-\x1f\x7f]/;
    $state->{pos} = $end;
    return;
}

sub toml_space {
    my ( $state, $lines ) = @_;
    while ( $state->{pos} < $state->{length} ) {
        my $char = toml_peek($state);
        if ( $char eq ' ' || $char eq "\t" || ( $lines && $char eq "\n" ) ) {
            $state->{pos}++;
        }
        elsif ( $lines && $char eq '#' ) {
            toml_comment($state);
        }
        else {
            last;
        }
    }
    return;
}

sub toml_string {
    my ( $state, $key ) = @_;
    my $quote     = toml_peek($state);
    my $multiline = toml_peek( $state, 3 ) eq $quote x 3;
    toml_die( $state, 'multiline keys are not permitted' )
      if $key && $multiline;
    $state->{pos} += $multiline ? 3 : 1;
    $state->{pos}++ if $multiline && toml_peek($state) eq "\n";
    my $value = '';
    while ( $state->{pos} < $state->{length} ) {
        my $char = toml_peek($state);
        if ( $char eq $quote ) {
            my $count = 1;
            $count++
              while substr( $state->{text}, $state->{pos} + $count, 1 ) eq
              $quote;
            if ( !$multiline ) {
                $state->{pos}++;
                return $value;
            }
            if ( $count >= 3 ) {
                toml_die( $state, 'too many closing quotes' ) if $count > 5;
                $value .= $quote x ( $count - 3 );
                $state->{pos} += $count;
                return $value;
            }
            $value .= $quote x $count;
            $state->{pos} += $count;
            next;
        }
        $state->{pos}++;
        if ( $quote eq '"' && $char eq '\\' ) {
            toml_die( $state, 'unterminated string escape' )
              if $state->{pos} >= $state->{length};
            if ($multiline) {
                my $next = $state->{pos};
                $next++ while substr( $state->{text}, $next, 1 ) =~ /[ \t]/;
                if ( substr( $state->{text}, $next, 1 ) eq "\n" ) {
                    $state->{pos} = $next + 1;
                    toml_space( $state, 0 );
                    while ( toml_peek($state) eq "\n" ) {
                        $state->{pos}++;
                        toml_space( $state, 0 );
                    }
                    next;
                }
            }
            my $escape = toml_peek($state);
            $state->{pos}++;
            my %escapes = (
                b    => "\b",
                t    => "\t",
                n    => "\n",
                f    => "\f",
                r    => "\r",
                '"'  => '"',
                '\\' => '\\'
            );
            if ( exists $escapes{$escape} ) {
                $value .= $escapes{$escape};
            }
            elsif ( $escape eq 'u' || $escape eq 'U' ) {
                my $digits = $escape eq 'u' ? 4 : 8;
                my $hex    = toml_peek( $state, $digits );
                toml_die( $state, 'invalid Unicode escape' )
                  unless length($hex) == $digits && $hex =~ /\A[0-9A-Fa-f]+\z/;
                my $point = hex($hex);
                toml_die( $state, 'invalid Unicode scalar value' )
                  if $point > 0x10ffff
                  || ( $point >= 0xd800 && $point <= 0xdfff );
                $value .= chr($point);
                $state->{pos} += $digits;
            }
            else {
                toml_die( $state, 'invalid string escape' );
            }
            next;
        }
        toml_die( $state, 'invalid control character in string' )
          if ( ord($char) < 32
            && $char ne "\t"
            && !( $multiline && $char eq "\n" ) )
          || ord($char) == 127;
        $value .= $char;
    }
    toml_die( $state, 'unterminated string' );
}

sub toml_keys {
    my ($state) = @_;
    my @keys;
    while (1) {
        toml_space( $state, 0 );
        my $char = toml_peek($state);
        if ( $char eq '"' || $char eq "'" ) {
            push @keys, toml_string( $state, 1 );
        }
        else {
            my $start = $state->{pos};
            $state->{pos}++ while toml_peek($state) =~ /[A-Za-z0-9_-]/;
            toml_die( $state, 'expected a TOML key' )
              if $start == $state->{pos};
            push @keys,
              substr( $state->{text}, $start, $state->{pos} - $start );
        }
        toml_space( $state, 0 );
        last unless toml_peek($state) eq '.';
        $state->{pos}++;
    }
    return \@keys;
}

sub toml_metadata {
    my ( $state, $table ) = @_;
    my $address = Scalar::Util::refaddr($table);
    $state->{tables}{$address} ||= {};
    return $state->{tables}{$address};
}

sub toml_descend {
    my ( $state, $table, $keys, $header ) = @_;
    for my $key ( @{$keys} ) {
        toml_die( $state, 'cannot extend an inline table' )
          if toml_metadata( $state, $table )->{frozen};
        if ( !exists $table->{$key} ) {
            $table->{$key} = {};
            toml_metadata( $state, $table->{$key} )->{dotted} = 1
              unless $header;
        }
        toml_die( $state, 'key is not a table' )
          unless ref( $table->{$key} ) eq 'HASH';
        $table = $table->{$key};
    }
    return $table;
}

sub toml_set {
    my ( $state, $table, $keys, $value ) = @_;
    my @parents = @{$keys};
    my $last    = pop @parents;
    $table = toml_descend( $state, $table, \@parents, 0 );
    toml_die( $state, 'cannot extend an inline table' )
      if toml_metadata( $state, $table )->{frozen};
    toml_die( $state, 'duplicate TOML key' ) if exists $table->{$last};
    $table->{$last} = $value;
    return;
}

sub toml_freeze {
    my ( $state, $table ) = @_;
    toml_metadata( $state, $table )->{frozen} = 1;
    for my $value ( values %{$table} ) {
        toml_freeze( $state, $value ) if ref($value) eq 'HASH';
    }
    return;
}

sub toml_value {
    my ( $state, $depth ) = @_;
    toml_die( $state, 'configuration nesting limit exceeded' ) if $depth > 128;
    toml_space( $state, 0 );
    my $char = toml_peek($state);
    return toml_string( $state, 0 ) if $char eq '"' || $char eq "'";
    if ( $char eq '[' ) {
        $state->{pos}++;
        my @items;
        toml_space( $state, 1 );
        while ( toml_peek($state) ne ']' ) {
            push @items, toml_value( $state, $depth + 1 );
            toml_space( $state, 1 );
            last if toml_peek($state) eq ']';
            toml_die( $state, 'expected comma in array' )
              unless toml_peek($state) eq ',';
            $state->{pos}++;
            toml_space( $state, 1 );
        }
        $state->{pos}++;
        return \@items;
    }
    if ( $char eq '{' ) {
        $state->{pos}++;
        my $table = {};
        toml_space( $state, 0 );
        while ( toml_peek($state) ne '}' ) {
            my $keys = toml_keys($state);
            toml_die( $state, 'expected equals in inline table' )
              unless toml_peek($state) eq '=';
            $state->{pos}++;
            toml_set( $state, $table, $keys, toml_value( $state, $depth + 1 ) );
            toml_space( $state, 0 );
            last if toml_peek($state) eq '}';
            toml_die( $state, 'expected comma in inline table' )
              unless toml_peek($state) eq ',';
            $state->{pos}++;
            toml_space( $state, 0 );
            toml_die( $state, 'trailing inline-table comma is not permitted' )
              if toml_peek($state) eq '}';
        }
        $state->{pos}++;
        toml_freeze( $state, $table );
        return $table;
    }
    my $start = $state->{pos};
    $state->{pos}++
      while toml_peek($state) ne '' && toml_peek($state) !~ /[ \t\n,\]\}#]/;
    my $token = substr( $state->{text}, $start, $state->{pos} - $start );
    return toml_boolean(1) if $token eq 'true';
    return toml_boolean(0) if $token eq 'false';
    my ( $digits, $base, $negative );
    if ( $token =~ /\A([+-]?)(0|[1-9](?:_?[0-9])*)\z/ ) {
        ( $negative, $digits, $base ) = ( $1 eq '-', $2, 10 );
    }
    elsif ( $token =~ /\A0([xob])([0-9A-Fa-f](?:_?[0-9A-Fa-f])*)\z/ ) {
        my $kind = $1;
        $digits = $2;
        $base   = $kind eq 'x' ? 16 : $kind eq 'o' ? 8 : 2;
        my $alphabet =
          $base == 2 ? qr/[01]/ : $base == 8 ? qr/[0-7]/ : qr/[0-9A-Fa-f]/;
        ( my $without = $digits ) =~ s/_//g;
        toml_die( $state, 'invalid integer digit' )
          if $without =~ /[^0-9A-Fa-f]/
          || grep { $_ !~ /\A$alphabet\z/ } split //, $without;
        $negative = 0;
    }
    else {
        toml_die( $state, 'unsupported or invalid TOML value' );
    }
    $digits =~ s/_//g;

    # The only integer setting is bounded by 1000. Saturating after 1001
    # preserves every schema decision and avoids overflow or huge bignums.
    my $value = 0;
    for my $digit ( split //, lc $digits ) {
        my $number = index( '0123456789abcdef', $digit );
        $value = $value * $base + $number;
        if ( $value > 1000 ) {
            $value = 1001;
            last;
        }
    }
    return toml_integer( $negative ? -$value : $value );
}

sub parse_toml {
    my ($text) = @_;
    my $state =
      { text => $text, pos => 0, length => length($text), tables => {} };
    my $root    = {};
    my $current = $root;
    toml_metadata( $state, $root )->{explicit} = 1;
    while (1) {
        toml_space( $state, 1 );
        last if $state->{pos} == $state->{length};
        if ( toml_peek($state) eq '[' ) {
            $state->{pos}++;
            my $keys = toml_keys($state);
            toml_die( $state, 'expected closing table bracket' )
              unless toml_peek($state) eq ']';
            $state->{pos}++;
            $current = toml_descend( $state, $root, $keys, 1 );
            my $metadata = toml_metadata( $state, $current );
            toml_die( $state, 'duplicate table declaration' )
              if $metadata->{explicit}
              || $metadata->{dotted}
              || $metadata->{frozen};
            $metadata->{explicit} = 1;
        }
        else {
            my $keys = toml_keys($state);
            toml_die( $state, 'expected equals after key' )
              unless toml_peek($state) eq '=';
            $state->{pos}++;
            toml_set( $state, $current, $keys, toml_value( $state, 0 ) );
        }
        toml_space( $state, 0 );
        if ( toml_peek($state) eq '#' ) {
            toml_comment($state);
        }
        toml_die( $state, 'unexpected text after statement' )
          if $state->{pos} < $state->{length} && toml_peek($state) ne "\n";
    }
    return $root;
}

# Path and glob helpers. fnmatchcase is implemented directly so that '*'
# and '?' can cross '/' exactly as they do in the contract tests.
#

my %GLOB_REGEX_CACHE;

sub fnmatch_pattern {
    my ($pattern) = @_;
    return $GLOB_REGEX_CACHE{$pattern} if exists $GLOB_REGEX_CACHE{$pattern};
    my @pieces = ('');
    my $index  = 0;
    while ( $index < length($pattern) ) {
        my $character = substr( $pattern, $index++, 1 );
        if ( $character eq '*' ) {
            push @pieces, '' if @pieces == 1 || $pieces[-1] ne '';
            next;
        }
        if ( $character eq '?' ) { $pieces[-1] .= '.'; next; }
        if ( $character eq '[' ) {
            my $end = $index;
            $end++ if substr( $pattern, $end, 1 ) eq '!';
            $end++ if substr( $pattern, $end, 1 ) eq ']';
            $end++
              while $end < length($pattern)
              && substr( $pattern, $end, 1 ) ne ']';
            if ( $end >= length($pattern) ) {
                $pieces[-1] .= '\[';
                next;
            }
            my $class = substr( $pattern, $index, $end - $index );
            $index = $end + 1;
            my $negative = $class =~ s/\A!//;
            my @chars    = split //, $class;
            my $members  = '';
            while (@chars) {
                my $start = shift @chars;
                if ( @chars > 1 && $chars[0] eq '-' ) {
                    shift @chars;
                    my $last = shift @chars;

                    # fnmatch treats a descending range as an empty set.
                    $members .= quotemeta($start) . '-' . quotemeta($last)
                      if ord($start) <= ord($last);
                }
                else {
                    $members .= quotemeta($start);
                }
            }
            $pieces[-1] .=
                $members ne '' ? '[' . ( $negative ? '^' : '' ) . $members . ']'
              : $negative      ? '.'
              :                  '(?!)';
            next;
        }
        $pieces[-1] .= quotemeta($character);
    }
    my $regex = shift @pieces;

    # Match each interior star minimally and atomically, as fnmatch does,
    # rather than allowing exponential regex backtracking over many stars.
    while ( @pieces > 1 ) {
        $regex .= '(?>.*?' . shift(@pieces) . ')';
    }
    $regex .= '.*' . $pieces[0] if @pieces;
    my $compiled = qr/\A$regex\z/s;
    $GLOB_REGEX_CACHE{$pattern} = $compiled;
    return $compiled;
}

sub fnmatchcase {
    my ( $path, $pattern ) = @_;
    return $path =~ fnmatch_pattern($pattern);
}

sub pure_parts {
    my ($path) = @_;
    my @parts  = grep { $_ ne '' && $_ ne '.' } split m{/}, $path;
    return @parts;
}

sub path_name {
    my ($path) = @_;
    my @parts = pure_parts($path);
    return @parts ? $parts[-1] : '';
}

sub path_parent_name {
    my ($path) = @_;
    my @parts = pure_parts($path);
    return $parts[-2] // '' if @parts >= 2;
    return '';
}

sub path_suffix {
    my ($path) = @_;
    my $name = path_name($path);
    return '' if $name eq '' || $name eq '.' || $name eq '..';
    my $index = rindex( $name, '.' );
    return '' if $index <= 0;
    return substr( $name, $index );
}

sub path_is_absolute {
    my ($path) = @_;
    return $path =~ m{^/};
}

sub path_has_parent_traversal {
    my ($path) = @_;
    return scalar grep { $_ eq '..' } pure_parts($path);
}

sub is_instruction {
    my ( $path, @extra_globs ) = @_;
    my $name = path_name($path);
    return 1 if $INSTRUCTION_NAMES{$name};
    for my $suffix (@INSTRUCTION_SUFFIXES) {
        return 1
          if length($name) >= length($suffix)
          && substr( $name, -length($suffix) ) eq $suffix;
    }
    my @parts       = pure_parts($path);
    my $parent_name = path_parent_name($path);
    return 1
      if $parent_name eq 'rules'
      && grep { $_ eq '.cursor' || $_ eq '.windsurf' || $_ eq '.continue' }
      @parts;
    return 1 if $path eq '.agents/skills/README.md';
    for my $glob (@extra_globs) {
        return 1 if fnmatchcase( $path, $glob );
    }
    return 0;
}

sub is_excluded {
    my ( $path, @patterns ) = @_;
    for my $pattern (@patterns) {
        return 1 if fnmatchcase( $path, $pattern );
    }
    return 0;
}

sub read_utf8 {
    my ($path) = @_;
    open my $handle, '<:raw', $path or die "$path: $!\n";
    local $/;
    local $! = 0;
    my $bytes = <$handle>;
    my $error = $!;
    close $handle or die "$path: $!\n";
    die "$path: $error\n" if $error;
    $bytes = '' unless defined $bytes;
    my $text = decode( 'UTF-8', $bytes, FB_CROAK );
    $text =~ s/\r\n?/\n/g;
    return $text;
}

sub resolve_path {
    my ($path) = @_;
    die "NUL byte in repository reference\n" if index( $path, "\0" ) >= 0;
    my $absolute = filesystem_text( File::Spec->rel2abs($path) );
    my @pending  = split m{/+}, $absolute;
    my @resolved;
    my $links = 0;
    while (@pending) {
        my $part = shift @pending;
        next if $part eq '' || $part eq '.';
        if ( $part eq '..' ) {
            pop @resolved if @resolved;
            next;
        }
        my $candidate = '/' . join( '/', @resolved, $part );
        if ( -l $candidate ) {

            # Resolve links before lexical '..', including dangling links.
            # A bound prevents cyclic or expanding links from looping forever.
            die "repository reference has too many symbolic links\n"
              if ++$links > 256;
            my $target = readlink($candidate);
            die "cannot read symbolic link: $candidate: $!\n"
              unless defined $target;
            $target   = filesystem_text($target);
            @resolved = () if substr( $target, 0, 1 ) eq '/';
            unshift @pending, split m{/+}, $target;
            next;
        }
        push @resolved, $part;
    }
    return '/' . join( '/', @resolved );
}

sub path_is_within {
    my ( $candidate, $root ) = @_;
    return 1 if $candidate eq $root;
    return 1 if index( $candidate, $root . '/' ) == 0;
    return 0;
}

sub unquote_path {
    my ($value) = @_;
    return $value unless $value =~ /%/;
    my @segments = split /(%[0-9A-Fa-f]{2})/, $value, -1;
    my $result   = '';
    my @bytes;
    my $flush = sub {
        return unless @bytes;
        $result .= decode( 'UTF-8', pack( 'C*', @bytes ), FB_DEFAULT );
        @bytes = ();
    };
    for my $segment (@segments) {
        next if $segment eq '';
        if ( $segment =~ /^%([0-9A-Fa-f]{2})$/ ) {
            push @bytes, hex($1);
        }
        else {
            $flush->();
            $result .= $segment;
        }
    }
    $flush->();
    return $result;
}

sub split_url {
    my ($destination) = @_;
    my ( $scheme, $rest ) = ( '', $destination );
    if ( $rest =~ /^([A-Za-z][A-Za-z0-9+.-]*):/ ) {
        $scheme = $1;
        $rest   = substr( $rest, length($1) + 1 );
    }
    my $netloc = '';
    if ( $rest =~ m{^//} ) {
        $rest =~ s{^//}{};
        my ( $authority, $tail ) = split m{/}, $rest, 2;
        $netloc = defined $authority ? $authority  : '';
        $rest   = defined $tail      ? '/' . $tail : '';
    }
    my $path = $rest;
    $path =~ s/[?#].*$//s;
    return ( $scheme, $netloc, $path );
}

#
# Repository configuration.
#

sub default_config {
    return {
        exclude_paths            => [],
        instruction_globs        => [],
        required_headings        => {},
        required_phrases         => {},
        strict_text              => {},
        skill_reference_prefixes => [],
        inline_path_checks       => 0,
        minimum_changed_lines    => 1,
    };
}

sub config_error {
    my ($message) = @_;
    die "$CONFIG_NAME: $message\n";
}

sub is_plain_string {
    my ($value) = @_;
    return defined $value && !ref $value;
}

sub is_array_value {
    my ($value) = @_;
    return ref($value) eq 'ARRAY';
}

sub is_table_value {
    my ($value) = @_;
    return ref($value) eq 'HASH';
}

sub is_boolean_value {
    my ($value) = @_;
    return ref($value) eq 'AKV::Bool';
}

sub integer_value {
    my ($value) = @_;
    return AKV::Int::value($value)  if ref($value) eq 'AKV::Int';
    return AKV::Bool::value($value) if ref($value) eq 'AKV::Bool';
    return;
}

sub validate_relative_config_path {
    my ( $key, $path ) = @_;
    if ( path_is_absolute($path) || path_has_parent_traversal($path) ) {
        config_error("$key entries must be repository-relative: $path");
    }
    return;
}

sub parse_config {
    my ($root) = @_;
    my $path = "$root/$CONFIG_NAME";
    return default_config() unless -e $path;

    my $text = '';
    my $parsed;
    my $error;
    eval {
        $text   = read_utf8($path);
        $parsed = parse_toml($text);
    };
    if ($@) {
        $error = $@;
        $error =~ s/\s+$//;
        $error =~ s/^$CONFIG_NAME: //;
        config_error($error);
    }

    my @root_keys = grep { $_ ne 'validator' } keys %{$parsed};
    if (@root_keys) {
        config_error('only the [validator] table is supported');
    }
    my $settings = $parsed->{validator};
    $settings = {} unless defined $settings;
    if ( !is_table_value($settings) ) {
        config_error('[validator] must be a table');
    }

    my %allowed = map { $_ => 1 } (
        'contract_version',         'exclude_paths',
        'instruction_globs',        'required_headings',
        'required_phrases',         'strict_text',
        'skill_reference_prefixes', 'inline_path_checks',
        'minimum_changed_lines',
    );
    my @unknown = sort grep { !$allowed{$_} } keys %{$settings};
    if (@unknown) {
        config_error( 'unknown setting(s): ' . join( ', ', @unknown ) );
    }

    my $version = $settings->{contract_version};
    $version = $CONTRACT_VERSION unless defined $version;
    if ( !is_plain_string($version) || $version ne $CONTRACT_VERSION ) {
        config_error("contract_version must be '$CONTRACT_VERSION'");
    }

    my %parsed_config;
    for my $key ( 'exclude_paths', 'instruction_globs' ) {
        my $items = $settings->{$key};
        $items = [] unless defined $items;
        if ( !is_array_value($items)
            || grep { !is_plain_string($_) || trim_text($_) eq '' } @{$items} )
        {
            config_error("$key must be an array of non-empty strings");
        }
        for my $item ( @{$items} ) {
            validate_relative_config_path( $key, $item );
        }
        $parsed_config{$key} = $items;
    }

    for my $key ( 'required_headings', 'required_phrases' ) {
        my $mapping = $settings->{$key};
        $mapping = {} unless defined $mapping;
        if ( !is_table_value($mapping) ) {
            config_error("$key must be a table of paths to arrays");
        }
        my %validated;
        for my $mapping_path ( keys %{$mapping} ) {
            if (   path_is_absolute($mapping_path)
                || path_has_parent_traversal($mapping_path) )
            {
                config_error(
                    "$key paths must be repository-relative: $mapping_path");
            }
            my $values = $mapping->{$mapping_path};
            if ( !is_array_value($values)
                || grep { !is_plain_string($_) || trim_text($_) eq '' }
                @{$values} )
            {
                config_error(
                    "$key.$mapping_path must be an array of non-empty strings");
            }
            $validated{$mapping_path} = $values;
        }
        $parsed_config{$key} = \%validated;
    }

    my $known_paths = {};
    for my $instruction_path (
        discover_instruction_files( $root, $parsed_config{instruction_globs} ) )
    {
        $known_paths->{$instruction_path} = 1;
    }
    for my $key ( 'required_headings', 'required_phrases' ) {
        my @unknown_paths =
          sort grep { !$known_paths->{$_} } keys %{ $parsed_config{$key} };
        if (@unknown_paths) {
            config_error( "$key references unknown instruction path(s): "
                  . join( ', ', @unknown_paths ) );
        }
    }

    my $strict_text = $settings->{strict_text};
    $strict_text = {} unless defined $strict_text;
    if ( !is_table_value($strict_text) ) {
        config_error('strict_text must be a table');
    }
    my %allowed_text_rules = map { $_ => 1 } (
        'ascii_only',    'lf_only',
        'final_newline', 'no_trailing_whitespace',
        'no_nul',
    );
    my %parsed_strict;
    for my $pattern ( keys %{$strict_text} ) {
        if ( path_is_absolute($pattern) || path_has_parent_traversal($pattern) )
        {
            config_error("invalid strict_text pattern: $pattern");
        }
        my $rules = $strict_text->{$pattern};
        if ( !is_array_value($rules)
            || grep { !is_plain_string($_) || !$allowed_text_rules{$_} }
            @{$rules} )
        {
            config_error("strict_text.$pattern has an unknown rule");
        }
        $parsed_strict{$pattern} = $rules;
    }
    $parsed_config{strict_text} = \%parsed_strict;

    my $prefixes = $settings->{skill_reference_prefixes};
    $prefixes = [] unless defined $prefixes;
    if ( !is_array_value($prefixes)
        || grep { !is_plain_string($_) || $_ eq '' } @{$prefixes} )
    {
        config_error('skill_reference_prefixes must be an array');
    }
    $parsed_config{skill_reference_prefixes} = $prefixes;

    my $inline_checks = $settings->{inline_path_checks};
    $inline_checks = toml_boolean(0) unless defined $inline_checks;
    if ( !is_boolean_value($inline_checks) ) {
        config_error('inline_path_checks must be a boolean');
    }
    $parsed_config{inline_path_checks} = $inline_checks->value();

    my $minimum_lines = $settings->{minimum_changed_lines};
    $minimum_lines = toml_integer(1) unless defined $minimum_lines;
    my $minimum_value = integer_value($minimum_lines);
    if (   !defined $minimum_value
        || $minimum_value < 1
        || $minimum_value > 1000 )
    {
        config_error('minimum_changed_lines must be an integer from 1 to 1000');
    }
    $parsed_config{minimum_changed_lines} = $minimum_value;

    return \%parsed_config;
}

#
# Change discovery and instruction analysis.
#

sub parse_name_status {
    my ($raw)  = @_;
    my @fields = split /\x00/, $raw, -1;
    my @changes;
    my $index = 0;
    while ( $index + 1 < @fields ) {
        my $status = $fields[$index];
        my $path   = $fields[ $index + 1 ];
        $index += 2;
        if ( $status ne '' && $path ne '' ) {
            push @changes,
              {
                status => substr( $status, 0, 1 ),
                path   => $path,
              };
        }
    }
    return @changes;
}

sub deduplicate_changes {
    my (@changes) = @_;
    my %by_path;
    for my $change (@changes) {
        $by_path{ $change->{path} } = $change;
    }
    return map { $by_path{$_} } sort keys %by_path;
}

sub discover_changes {
    my ( $root, $scope, $base, $head ) = @_;
    if ( $scope eq 'worktree' ) {
        my $tracked =
          run_git( $root, 'diff', '--name-status', '-z', '--no-renames',
            'HEAD' );
        my @changes = parse_name_status($tracked);
        my $untracked =
          run_git( $root, 'ls-files', '--others', '--exclude-standard', '-z' );
        push @changes, map { { status => 'A', path => $_ } }
          grep { $_ ne '' } split /\x00/, $untracked, -1;
        return deduplicate_changes(@changes);
    }
    die "range scope requires base and head\n"
      unless defined $base && defined $head;
    my $base_commit = resolve_commit( $root, $base );
    my $head_commit = resolve_commit( $root, $head );
    my $merge_base =
      trim_text( run_git( $root, 'merge-base', $base_commit, $head_commit ) );
    my $raw = run_git( $root, 'diff', '--name-status', '-z', '--no-renames',
        $merge_base, $head_commit );
    return deduplicate_changes( parse_name_status($raw) );
}

sub changed_content {
    my ( $root, $scope, $base, $head, $path ) = @_;
    my $diff;
    if ( $scope eq 'range' ) {
        die "range scope requires base and head\n"
          unless defined $base && defined $head;
        my $base_commit = resolve_commit( $root, $base );
        my $head_commit = resolve_commit( $root, $head );
        my $merge_base =
          trim_text(
            run_git( $root, 'merge-base', $base_commit, $head_commit ) );
        $diff = run_git( $root, 'diff', '--no-ext-diff', '--unified=0',
            $merge_base, $head_commit, '--', $path );
    }
    else {
        $diff = run_git( $root, 'diff', '--no-ext-diff', '--unified=0',
            'HEAD', '--', $path );
    }

    my ( @added, @removed );
    for my $line ( split_text_lines($diff) ) {
        next if $line =~ /^(?:\+\+\+|---)/;
        if ( $line =~ /^\+(.*)$/s ) {
            my $normalized = normalize_knowledge_line($1);
            push @added, $normalized if $normalized ne '';
        }
        elsif ( $line =~ /^-(.*)$/s ) {
            my $normalized = normalize_knowledge_line($1);
            push @removed, $normalized if $normalized ne '';
        }
    }

    if ( $scope eq 'worktree' && $diff eq '' ) {
        my %changed = map { $_->{path} => 1 }
          discover_changes( $root, $scope, $base, $head );
        if ( $changed{$path} ) {
            my $candidate = "$root/$path";
            if ( -f $candidate ) {
                my ( $stdout, $stderr, $status ) =
                  run_git_optional( $root, 'ls-files', '--error-unmatch',
                    '--', $path );
                if ( $stdout eq '' ) {
                    my $content = eval { read_utf8($candidate) };
                    if ( defined $content ) {
                        for my $line ( split_text_lines($content) ) {
                            my $normalized = normalize_knowledge_line($line);
                            push @added, $normalized
                              if $normalized ne '';
                        }
                    }
                }
            }
        }
    }
    return ( \@added, \@removed );
}

sub normalize_knowledge_line {
    my ($line) = @_;
    my $content = $line;
    $content =~ s/^\s+//;
    $content =~ s/\s+$//;
    return '' if $content eq '';
    return ''
      if $content =~ /^#/
      || $content =~ /^<!--/
      || $content =~ /^-->/;
    return ''
      if $content =~
      /^(?:updated?|generated|last[ -]updated|date|updated_at)\s*[:=]/i;
    return '' if $content =~ /^[-*_`~>| ]+$/;
    $content              =~ s/\s+/ /g;
    $content              =~ s/^\s+//;
    $content              =~ s/\s+$//;
    return $content;
}

sub update_metrics {
    my ( $added, $removed ) = @_;
    my @remaining_added = @{$added};
    my @remaining_removed;
    for my $old ( @{$removed} ) {
        my $found = 0;
        for my $index ( 0 .. $#remaining_added ) {
            if ( $remaining_added[$index] eq $old ) {
                splice @remaining_added, $index, 1;
                $found = 1;
                last;
            }
        }
        push @remaining_removed, $old unless $found;
    }
    my @changed         = ( @remaining_added, @remaining_removed );
    my $effective_chars = 0;
    for my $line (@changed) {
        $line =~ s/[^A-Za-z0-9]//g;
        $effective_chars += length($line);
    }
    return ( scalar @changed, $effective_chars );
}

sub substantive_update {
    my ( $added, $removed, $minimum_lines ) = @_;
    my ( $line_count, $effective_chars ) = update_metrics( $added, $removed );
    return $line_count >= $minimum_lines && $effective_chars >= 24;
}

sub discover_instruction_files {
    my ( $root, $extra_globs ) = @_;
    my @found;
    my @pending = ('');

    while (@pending) {
        my $relative_directory = pop @pending;
        my $absolute_directory =
            $relative_directory eq ''
          ? $root
          : "$root/$relative_directory";
        opendir my $handle, $absolute_directory or next;
        my @entries = sort grep { $_ ne '.' && $_ ne '..' }
          map { decode( 'UTF-8', $_, FB_DEFAULT ) } readdir $handle;
        closedir $handle;

        my @directories;
        for my $entry (@entries) {
            my $absolute = "$absolute_directory/$entry";
            my $relative =
                $relative_directory eq ''
              ? $entry
              : "$relative_directory/$entry";
            if ( -l $absolute ) {
                next;
            }
            if ( -d $absolute ) {
                next if $entry eq '.git';
                push @directories, $relative;
                next;
            }
            if ( -f $absolute
                && is_instruction( $relative, @{$extra_globs} ) )
            {
                push @found, $relative;
            }
        }
        push @pending, reverse @directories;
    }
    my @sorted = sort @found;
    return @sorted;
}

sub parse_frontmatter_fields {
    my ($text) = @_;
    return
      unless $text =~ /\A---\s*\n(.*?)\n---\s*(?:\n|\z)/s;
    my $body = $1;
    my %fields;
    for my $line ( split_text_lines($body) ) {
        if ( $line =~ /^([A-Za-z0-9_-]+):\s*(.*?)\s*$/ ) {
            my ( $key, $value ) = ( $1, $2 );
            $value =~ s/^["']+//;
            $value =~ s/["']+$//;
            $fields{$key} = $value;
        }
    }
    return \%fields;
}

sub has_heading {
    my ( $text, $heading ) = @_;
    my $quoted = quotemeta($heading);
    return $text =~ /^##+\s+$quoted\s*$/m ? 1 : 0;
}

sub validate_structure {
    my ( $root, $paths, $required_headings, $required_phrases ) = @_;
    my @errors;
    for my $relative ( @{$paths} ) {
        my $text = eval { read_utf8("$root/$relative") };
        if ($@) {
            push @errors, [ $relative, 'cannot read UTF-8 instruction file' ];
            next;
        }
        if ( trim_text($text) eq '' ) {
            push @errors, [ $relative, 'instruction file is empty' ];
            next;
        }
        for my $heading ( @{ $required_headings->{$relative} // [] } ) {
            if ( !has_heading( $text, $heading ) ) {
                push @errors,
                  [ $relative, "missing configured section: $heading" ];
            }
        }
        for my $phrase ( @{ $required_phrases->{$relative} // [] } ) {
            my $normalized_text = $text;
            $normalized_text =~ s/\s+/ /g;
            my $normalized_phrase = $phrase;
            $normalized_phrase =~ s/\s+/ /g;
            if ( index( $normalized_text, $normalized_phrase ) < 0 ) {
                push @errors,
                  [ $relative, "missing configured policy text: $phrase" ];
            }
        }
        my $name = path_name($relative);
        if ( $name eq 'AGENTS.md' && $relative eq 'AGENTS.md' ) {
            if (
                !has_heading( $text, 'Continuous Agent & Skills Maintenance' ) )
            {
                push @errors,
                  [
                    $relative,
                    'missing Continuous Agent & Skills Maintenance section'
                  ];
            }
        }
        elsif ( $name eq 'SKILL.md' ) {
            my $fields = parse_frontmatter_fields($text);
            if ( !defined $fields ) {
                push @errors,
                  [
                    $relative,
                    'missing or malformed YAML frontmatter delimiters'
                  ];
            }
            else {
                for my $key ( 'name', 'description' ) {
                    my $value = $fields->{$key} // '';
                    if ( trim_text($value) eq '' ) {
                        push @errors,
                          [ $relative, "frontmatter requires non-empty $key" ];
                    }
                }
                my $field_name = $fields->{name} // '';
                if (   trim_text($field_name) ne ''
                    && trim_text($field_name) ne path_parent_name($relative) )
                {
                    push @errors,
                      [
                        $relative,
                        'frontmatter name must match the skill directory'
                      ];
                }
            }
            if ( !has_heading( $text, 'Maintenance & Continuous Learning' ) ) {
                push @errors,
                  [
                    $relative,
                    'missing Maintenance & Continuous Learning section'
                  ];
            }
        }
        elsif ( $name =~ /\.agent\.md$/ ) {
            if ( $text =~ /^---/ ) {
                my $fields = parse_frontmatter_fields($text);
                if ( !defined $fields ) {
                    push @errors,
                      [ $relative, 'malformed YAML frontmatter delimiters' ];
                }
                else {
                    my $field_name        = $fields->{name}        // '';
                    my $field_description = $fields->{description} // '';
                    if (   trim_text($field_name) eq ''
                        || trim_text($field_description) eq '' )
                    {
                        push @errors,
                          [
                            $relative,
                            'agent frontmatter requires name and description'
                          ];
                    }
                }
            }
            if (
                !has_heading( $text, 'Post-Change Knowledge Synchronization' ) )
            {
                push @errors,
                  [
                    $relative,
                    'missing Post-Change Knowledge Synchronization section'
                  ];
            }
        }
    }
    return @errors;
}

sub validate_extensions {
    my ( $root, $paths, $config, $skill_names ) = @_;
    my @errors;

    for my $relative ( @{$paths} ) {
        my $text = eval { read_utf8("$root/$relative") };
        next if $@;
        for my $pattern ( sort keys %{ $config->{strict_text} } ) {
            next unless fnmatchcase( $relative, $pattern );
            my %rules = map { $_ => 1 } @{ $config->{strict_text}{$pattern} };
            if ( $rules{lf_only} && $text =~ /\r/ ) {
                push @errors,
                  [ $relative,
                    'configured text rule requires LF line endings' ];
            }
            if (   $rules{final_newline}
                && $text ne ''
                && $text !~ /\n\z/ )
            {
                push @errors,
                  [ $relative,
                    'configured text rule requires a final newline' ];
            }
            if (   $rules{no_trailing_whitespace}
                && $text =~ /[\t ]+$/m )
            {
                push @errors,
                  [
                    $relative,
                    'configured text rule forbids trailing whitespace'
                  ];
            }
            if ( $rules{no_nul} && index( $text, "\x00" ) >= 0 ) {
                push @errors,
                  [ $relative, 'configured text rule forbids NUL bytes' ];
            }
            if ( $rules{ascii_only} && $text =~ /[^\x00-\x7f]/ ) {
                push @errors,
                  [ $relative, 'configured text rule requires ASCII text' ];
            }
        }
    }

    my @prefixes = @{ $config->{skill_reference_prefixes} };
    if (@prefixes) {
        for my $relative ( @{$paths} ) {
            next unless $relative =~ /\.agent\.md\z/;
            my $text = eval { read_utf8("$root/$relative") };
            next if $@;
            while ( $text =~ /`([^`]+)`/g ) {
                my $reference = $1;
                my $matches_prefix =
                  scalar grep { index( $reference, $_ ) == 0 } @prefixes;
                if ( $matches_prefix && !$skill_names->{$reference} ) {
                    push @errors,
                      [ $relative, "references missing skill: $reference" ];
                }
            }
        }
    }

    if ( $config->{inline_path_checks} ) {
        my %path_suffixes =
          map { $_ => 1 } ( '.md', '.pl', '.pm', '.t', '.8', '.ignore' );
        for my $relative ( @{$paths} ) {
            my $text = eval { read_utf8("$root/$relative") };
            next if $@;
            while ( $text =~ /`([^`]+)`/g ) {
                my $target = $1;
                next if $target =~ m{^/};
                next if $target =~ /\s/;
                my $suffix = path_suffix($target);
                next
                  unless $path_suffixes{$suffix}
                  || $target eq 'Makefile'
                  || $target eq 'LICENSE';
                my $parent_directory = dirname("$root/$relative");
                my $first_candidate  = "$parent_directory/$target";
                my $second_candidate = "$root/$target";

                if ( !-e $first_candidate && !-e $second_candidate ) {
                    push @errors,
                      [
                        $relative,
                        "inline repository path does not exist: $target"
                      ];
                }
            }
        }
    }
    return @errors;
}

sub validate_references {
    my ( $root, $paths ) = @_;
    my @errors;
    my $root_resolved = realpath($root);
    $root_resolved = abs_path($root) unless defined $root_resolved;
    $root_resolved = $root           unless defined $root_resolved;
    $root_resolved = filesystem_text($root_resolved);

    for my $relative ( @{$paths} ) {
        my $file_path = "$root/$relative";
        my $text      = eval { read_utf8($file_path) };
        next if $@;
        while ( $text =~ /(?<!!)\[[^\]]*\]\(([^)]+)\)/g ) {
            my $target = $1;
            $target =~ s/^\s+//;
            $target =~ s/\s+$//;
            my ($destination) = split /\s+/, $target, 2;
            next unless defined $destination;
            $destination =~ s/^<+//;
            $destination =~ s/>+$//;
            my ( $scheme, $netloc, $url_path ) = split_url($destination);
            next if $scheme ne '' || $netloc ne '';
            next if $url_path eq '';
            my $local_path = unquote_path($url_path);
            my $candidate;

            if ( $local_path =~ m{^/} ) {
                $local_path =~ s{^/+}{};
                $candidate = "$root/$local_path";
            }
            else {
                $candidate = dirname($file_path) . "/$local_path";
            }
            my $resolved = eval { resolve_path($candidate) };
            if ($@) {
                push @errors,
                  [ $relative, "cannot resolve local reference: $destination" ];
                next;
            }
            if ( !path_is_within( $resolved, $root_resolved ) ) {
                push @errors,
                  [
                    $relative,
                    "local reference escapes repository: $destination"
                  ];
                next;
            }
            if ( !-e $resolved ) {
                push @errors,
                  [ $relative, "broken local reference: $destination" ];
            }
        }
    }
    return @errors;
}

sub find_duplicate_or_conflicting {
    my ( $paths, $root ) = @_;
    my @duplicates;
    my @conflicts;
    my @by_content;
    my %content_index;

    for my $relative ( @{$paths} ) {
        my $text = eval { read_utf8("$root/$relative") };
        next if $@;
        my $normalized = $text;
        $normalized =~ s/\s+/ /g;
        $normalized =~ s/^\s+//;
        $normalized =~ s/\s+$//;
        if ( $normalized ne '' ) {
            if ( !exists $content_index{$normalized} ) {
                $content_index{$normalized} = scalar @by_content;
                push @by_content, [ $normalized, [] ];
            }
            push @{ $by_content[ $content_index{$normalized} ][1] }, $relative;
        }

        my %actions;
        my @action_order;
        for my $line ( split_text_lines($text) ) {
            my $prefix;
            my $modal;
            my $action_text;
            next
              unless $line =~
/^\s*(?<prefix>.*?)\b(?<modal>must not|mustn't|must|should not|should|do not|don't|never|always)\b\s+(?<action>[^.!?]{12,})[.!?]?\s*$/i;
            $prefix      = $+{prefix} // '';
            $modal       = lc( $+{modal} // '' );
            $action_text = lc( $prefix . ' ' . ( $+{action} // '' ) );
            $action_text =~ s/[^a-z0-9]+/ /g;
            $action_text =~ s/^\s+//;
            $action_text =~ s/\s+$//;
            next if length($action_text) < 12;
            my $polarity =
              (      $modal eq 'must not'
                  || $modal eq "mustn't"
                  || $modal eq 'should not'
                  || $modal eq 'do not'
                  || $modal eq "don't"
                  || $modal eq 'never' )
              ? 'negative'
              : 'positive';

            if ( !exists $actions{$action_text} ) {
                push @action_order, $action_text;
                $actions{$action_text} = {};
            }
            $actions{$action_text}{$polarity} = 1;
        }
        for my $action (@action_order) {
            my $polarities = $actions{$action};
            if ( $polarities->{negative} && $polarities->{positive} ) {
                push @conflicts,
                  "$relative: explicitly opposite directives for '$action'";
            }
        }
    }

    for my $group (@by_content) {
        my @files = sort @{ $group->[1] };
        push @duplicates, join( ', ', @files ) if @files > 1;
    }
    return ( \@duplicates, \@conflicts );
}

#
# Rule assembly and reporting.
#

sub make_result {
    my ( $rule_id, $status, $message, $files, $remediation ) = @_;
    $files = [] unless defined $files;
    my %seen;
    my @sorted = sort grep { !$seen{$_}++ } @{$files};
    return {
        rule_id     => $rule_id,
        status      => $status,
        message     => $message,
        files       => \@sorted,
        remediation => $remediation,
    };
}

sub is_infrastructure_only {
    my (@paths) = @_;
    return 0 unless @paths;
    for my $path (@paths) {
        my $matched = 0;
        for my $pattern (@INFRASTRUCTURE_PATTERNS) {
            if ( fnmatchcase( $path, $pattern ) ) {
                $matched = 1;
                last;
            }
        }
        return 0 unless $matched;
    }
    return 1;
}

sub json_string {
    my ($value) = @_;
    return JSON::PP->new->ascii(0)->encode($value);
}

sub run_validation {
    my ( $arguments, $cwd ) = @_;
    my ( $root, $config, $changes, $instruction_paths );
    my $setup_error;
    eval {
        $root    = find_repository_root($cwd);
        $config  = parse_config($root);
        $changes = [
            discover_changes(
                $root,              $arguments->{scope},
                $arguments->{base}, $arguments->{head}
            )
        ];
        $instruction_paths =
          [ discover_instruction_files( $root, $config->{instruction_globs} ) ];
    };
    if ($@) {
        $setup_error = $@;
        $setup_error =~ s/\s+$//;
        my $failure = make_result(
            'AK005',
            'FAIL',
            'Invalid validator configuration or execution error',
            [],
            'Check the Git worktree, revisions, and validator configuration.',
        );
        my @checks = (
            make_result(
                'AK001', 'PASS', 'Agent knowledge synchronization',
                [], undef
            ),
            make_result(
                'AK002', 'PASS', 'Agent and skill structure',
                [], undef
            ),
            make_result(
                'AKX001', 'PASS', 'Configured repository-specific validations',
                [], undef
            ),
            make_result( 'AK003', 'PASS', 'Internal references', [], undef ),
            make_result(
                'AK004', 'PASS', 'Knowledge update relevance',
                [], undef
            ),
            $failure,
            make_result(
                'AK006', 'PASS',
                'No duplicate or conflicting instructions detected',
                [], undef
            ),
        );
        return ( \@checks, 2, $setup_error );
    }

    my @excluded   = ( @DEFAULT_EXCLUDED_PATHS, @{ $config->{exclude_paths} } );
    my @considered = grep { !is_excluded( $_->{path}, @excluded ) } @{$changes};
    my @changed_paths       = map { $_->{path} } @considered;
    my $infrastructure_only = is_infrastructure_only(@changed_paths);

    my @knowledge_changes;
    my @project_changes;
    for my $change (@considered) {
        my $path = $change->{path};
        if ( is_instruction( $path, @{ $config->{instruction_globs} } ) ) {
            push @knowledge_changes, $path if -f "$root/$path";
        }
        else {
            push @project_changes, $path;
        }
    }

    my ( @substantive_files, @suspicious_files );
    for my $path (@knowledge_changes) {
        my ( $added, $removed ) =
          changed_content( $root,
            $arguments->{scope}, $arguments->{base}, $arguments->{head},
            $path );
        if (
            substantive_update(
                $added, $removed, $config->{minimum_changed_lines}
            )
          )
        {
            push @substantive_files, $path;
        }
        else {
            push @suspicious_files, $path;
        }
    }

    my $sync;
    if ( !@considered || $infrastructure_only ) {
        $sync = make_result( 'AK001', 'PASS', 'Agent knowledge synchronization',
            [], undef );
    }
    elsif (@substantive_files) {
        $sync = make_result( 'AK001', 'PASS', 'Agent knowledge synchronized',
            \@substantive_files, undef );
    }
    else {
        $sync = make_result(
            'AK001',
            'FAIL',
            'Missing agent knowledge update',
            ( @project_changes ? \@project_changes : \@changed_paths ),
'Update a relevant AGENTS.md, agent instruction, or SKILL.md with substantive operational guidance.',
        );
    }

    my @structure_errors = validate_structure(
        $root, $instruction_paths,
        $config->{required_headings},
        $config->{required_phrases}
    );
    my ( $structure, @structure_detail );
    if (@structure_errors) {
        $structure = make_result(
            'AK002',
            'FAIL',
            'Invalid agent or skill structure',
            [ map { $_->[0] } @structure_errors ],
'Fix the required headings and simple frontmatter fields; frontmatter checks are partial and are not full YAML validation.',
        );
        @structure_detail = map { [ $_->[0], $_->[1] ] } @structure_errors;
    }
    else {
        $structure =
          make_result( 'AK002', 'PASS', 'Agent and skill structure valid',
            [], undef );
    }

    my %skill_names;
    for my $relative ( @{$instruction_paths} ) {
        next unless path_name($relative) eq 'SKILL.md';
        my $text = eval { read_utf8("$root/$relative") };
        next if $@;
        my $fields = parse_frontmatter_fields($text);
        next unless defined $fields;
        my $name = trim_text( $fields->{name} // '' );
        $skill_names{$name} = 1 if $name ne '';
    }
    my @extension_errors =
      validate_extensions( $root, $instruction_paths, $config, \%skill_names );
    my ( $extension, @extension_detail );
    if (@extension_errors) {
        $extension = make_result(
            'AKX001',
            'FAIL',
            'Configured repository-specific validation failed',
            [ map { $_->[0] } @extension_errors ],
'Correct the configured rule or update its documented repository-specific configuration.',
        );
        @extension_detail =
          map { [ 'AKX001', $_->[1] ] } @extension_errors;
    }
    else {
        $extension =
          make_result( 'AKX001', 'PASS',
            'Configured repository-specific validations passed',
            [], undef );
    }

    my @reference_errors = validate_references( $root, $instruction_paths );
    my ( $references, @reference_detail );
    if (@reference_errors) {
        $references = make_result(
            'AK003',
            'FAIL',
            'Broken internal references',
            [ map { $_->[0] } @reference_errors ],
            'Correct or remove the broken repository-relative Markdown link.',
        );
        @reference_detail =
          map { [ $_->[0], $_->[1] ] } @reference_errors;
    }
    else {
        $references = make_result( 'AK003', 'PASS',
            'Internal references valid', [], undef );
    }

    my $relevance;
    if (@suspicious_files) {
        $relevance = make_result(
            'AK004',
            'FAIL',
            'Suspicious knowledge update: no effective content change detected',
            \@suspicious_files,
'Replace cosmetic or metadata-only edits with substantive operational guidance.',
        );
    }
    elsif (@substantive_files) {
        $relevance = make_result(
            'AK004',
            'WARN',
            'Manual relevance review recommended',
            \@substantive_files,
'Confirm that the instruction update is relevant and reusable; automation cannot prove semantic correctness.',
        );
    }
    else {
        $relevance =
          make_result( 'AK004', 'PASS',
            'No suspicious knowledge update detected',
            [], undef );
    }

    my $configuration =
      make_result( 'AK005', 'PASS',
        'Validator configuration and execution valid',
        [], undef );

    my ( $duplicates, $conflicts ) =
      find_duplicate_or_conflicting( $instruction_paths, $root );
    my ( $conflict_result, @conflict_detail );
    if ( @{$duplicates} || @{$conflicts} ) {
        my @files;
        for my $group ( @{$duplicates} ) {
            push @files, split /, /, $group;
        }
        for my $item ( @{$conflicts} ) {
            my ($path) = split /:/, $item, 2;
            push @files, $path;
        }
        my @detail;
        if ( @{$duplicates} ) {
            push @detail,
              'exact duplicate files: ' . join( '; ', @{$duplicates} );
        }
        push @detail, @{$conflicts};
        $conflict_result = make_result(
            'AK006',
            'WARN',
            'Potential duplicate or conflicting instructions found',
            \@files,
'Review the listed instructions and consolidate or clarify only when their scopes truly overlap.',
        );
        @conflict_detail = map { [ 'AK006', $_ ] } @detail;
    }
    else {
        $conflict_result =
          make_result( 'AK006', 'PASS',
            'No duplicate or conflicting instructions detected',
            [], undef, );
    }

    my @details = (
        @structure_detail, @extension_detail,
        @reference_detail, @conflict_detail,
    );
    my @results = (
        $sync,      $structure,     $extension, $references,
        $relevance, $configuration, $conflict_result,
    );
    return ( \@results, 0, json_string( \@details ) );
}

sub make_report {
    my ($checks) = @_;
    my @errors =
      grep { $_->{status} eq 'FAIL' } @{$checks};
    my @warnings =
      grep { $_->{status} eq 'WARN' } @{$checks};
    return {
        schema_version   => $SCHEMA_VERSION,
        contract_version => $CONTRACT_VERSION,
        status           => ( @errors ? 'FAIL' : 'PASS' ),
        summary          => {
            errors   => scalar @errors,
            warnings => scalar @warnings,
        },
        checks   => $checks,
        errors   => \@errors,
        warnings => \@warnings,
    };
}

sub print_report {
    my ( $report, $output_format ) = @_;
    if ( $output_format eq 'json' ) {
        my $encoder =
          JSON::PP->new->canonical(1)
          ->pretty(1)
          ->indent_length(2)
          ->space_before(0)
          ->space_after(1)
          ->ascii(0);
        print $encoder->encode($report);
        return;
    }
    for my $check ( @{ $report->{checks} } ) {
        print '[', $check->{status}, '] ', $check->{rule_id}, ' - ',
          $check->{message}, "\n";
        if ( @{ $check->{files} } ) {
            print '  Files: ', join( ', ', @{ $check->{files} } ), "\n";
        }
        if ( defined $check->{remediation} && $check->{remediation} ne '' ) {
            print '  Action: ', $check->{remediation}, "\n";
        }
    }
    print "\nAgent Knowledge Validation\n";
    print 'Status: ',   $report->{status},            "\n";
    print 'Errors: ',   $report->{summary}{errors},   "\n";
    print 'Warnings: ', $report->{summary}{warnings}, "\n";
    return;
}

#
# Command line.
#

my $PROGRAM = basename($0);

sub usage_text {
    return
        "usage: $PROGRAM [-h] --scope {worktree,range} [--base BASE]\n"
      . ( ' ' x ( length($PROGRAM) + 8 ) )
      . "[--head HEAD] [--format {text,json}]\n";
}

sub usage_error {
    my ($message) = @_;
    print STDERR usage_text(), "$PROGRAM: error: $message\n";
    exit 2;
}

sub argument_repr {
    my ($value) = @_;
    my $quote =
      index( $value, "'" ) >= 0 && index( $value, '"' ) < 0 ? '"' : "'";
    $value =~ s/\\/\\\\/g;
    $value =~ s/\Q$quote\E/\\$quote/g;
    $value =~ s/\n/\\n/g;
    $value =~ s/\r/\\r/g;
    $value =~ s/\t/\\t/g;
    $value =~
      s/([\x00-\x08\x0b\x0c\x0e-\x1f\x7f])/sprintf('\\x%02x', ord($1))/ge;
    return $quote . $value . $quote;
}

sub print_help {
    print usage_text(), "\n", <<"HELP";
Validate Agent Knowledge contract $CONTRACT_VERSION using only Perl's standard library.

options:
  -h, --help            show this help message and exit
  --scope {worktree,range}
  --base BASE           base Git revision; required with --scope range
  --head HEAD           head Git revision; required with --scope range
  --format {text,json}
HELP
    exit 0;
}

sub option_like {
    my ($value) = @_;
    return 0 if $value eq '-' || $value !~ /\A-/;
    return 0 if $value                  =~ /\A-(?:[0-9]+|[0-9]*\.[0-9]+)\z/;
    return 1;
}

sub parse_arguments {
    my %arguments = ( format => 'text' );
    my @unknown;
    my @options = qw(--help --scope --base --head --format);
    my $index   = 0;
    while ( $index < @ARGV ) {
        my $argument = $ARGV[ $index++ ];
        if ( $argument eq '--' ) {
            push @unknown, @ARGV[ $index .. $#ARGV ];
            last;
        }
        if ( $argument eq '-h' || $argument eq '--help' ) {
            print_help();
        }
        if ( $argument =~ /\A-h(.+)/s ) {
            usage_error( 'argument -h/--help: ignored explicit argument '
                  . argument_repr($1) );
        }
        my ( $name, $inline ) = split /=/, $argument, 2;
        if ( $name =~ /\A--/ && !grep { $_ eq $name } @options ) {
            my @matches = grep { index( $_, $name ) == 0 } @options;
            if ( @matches > 1 ) {
                usage_error( "ambiguous option: $name could match "
                      . join( ', ', @matches ) );
            }
            $name = $matches[0] if @matches;
        }
        if ( $name eq '--help' ) {
            usage_error( 'argument -h/--help: ignored explicit argument '
                  . argument_repr($inline) )
              if defined $inline;
            print_help();
        }
        if ( !grep { $_ eq $name } @options ) {
            push @unknown, $argument;
            next;
        }
        my $value;
        if ( defined $inline ) {
            $value = $inline;
        }
        elsif ( $index < @ARGV && !option_like( $ARGV[$index] ) ) {
            $value = $ARGV[ $index++ ];
        }
        else {
            usage_error("argument $name: expected one argument");
        }
        if ( $name eq '--scope' ) {
            usage_error( 'argument --scope: invalid choice: '
                  . argument_repr($value)
                  . " (choose from 'worktree', 'range')" )
              unless $value eq 'worktree' || $value eq 'range';
            $arguments{scope} = $value;
        }
        elsif ( $name eq '--format' ) {
            usage_error( 'argument --format: invalid choice: '
                  . argument_repr($value)
                  . " (choose from 'text', 'json')" )
              unless $value eq 'text' || $value eq 'json';
            $arguments{format} = $value;
        }
        elsif ( $name eq '--base' ) {
            $arguments{base} = $value;
        }
        else {
            $arguments{head} = $value;
        }
    }
    usage_error('the following arguments are required: --scope')
      unless defined $arguments{scope};
    usage_error( 'unrecognized arguments: ' . join( ' ', @unknown ) )
      if @unknown;
    if ( $arguments{scope} eq 'range' ) {
        if (   !defined $arguments{base}
            || $arguments{base} eq ''
            || !defined $arguments{head}
            || $arguments{head} eq '' )
        {
            usage_error('--scope range requires both --base and --head');
        }
    }
    elsif (( defined $arguments{base} && length $arguments{base} )
        || ( defined $arguments{head} && length $arguments{head} ) )
    {
        usage_error('--base and --head are only valid with --scope range');
    }
    return \%arguments;
}

sub main {
    binmode STDOUT, ':encoding(UTF-8)';
    binmode STDERR, ':encoding(UTF-8)';
    @ARGV = map { filesystem_text($_) } @ARGV;
    my $arguments = parse_arguments();
    my ( $checks, $execution_code, $detail ) =
      run_validation( $arguments, filesystem_text( getcwd() ) );
    my $report = make_report($checks);
    print_report( $report, $arguments->{format} );
    if ( $execution_code == 2 ) {
        if ( $arguments->{format} eq 'json' ) {
            print STDERR "validator execution error: $detail\n";
        }
        return 2;
    }
    return $report->{status} eq 'FAIL' ? 1 : 0;
}

exit main();
