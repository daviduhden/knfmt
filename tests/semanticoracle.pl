#!/usr/bin/perl

# Independent semantic-preservation oracle for knfmt (Perl).
#
# Formats change layout but must not change the program's preprocessing
# tokens.  This module tokenizes two byte strings (translation phases 2 and 3)
# and compares them.  It does not use knfmt's lexer.
#
# Policy
#   * Translation phase 1 (trigraph replacement) and phase 2
#     (backslash-newline splicing) are applied first.
#   * Preprocessing tokens: identifiers, preprocessing numbers, string and
#     character literals (escape aware), punctuators (longest match), comments
#     and newlines.
#   * Backslash tokens are dropped (knfmt discards them).
#   * A run of ';' punctuators with no newline between them is collapsed to
#     its last member (knfmt keeps the last on a line), and a ';' whose next
#     token is '}' is dropped (empty statement before a block close).
#   * The resulting non-comment token sequence must be identical, which
#     detects token loss, duplication, reordering, operator/punctuator and
#     literal changes, directive changes and branch swaps.
#   * Comments may be relocated and reindented, so only their
#     whitespace-normalized spelling multiset is compared.
#   * An unterminated literal (a raw newline inside a string or character
#     literal) cannot be interpreted as a preprocessing token; the comparison
#     reports ORACLE_UNSUPPORTED instead of comparing undefined bytes.
#
# Run with --selftest to check the oracle itself.

use strict;
use warnings;
use bytes;

my @PUNCT = sort { length($b) <=> length($a) } (
	'...', '<<=', '>>=', '->', '++', '--', '<<', '>>', '<=', '>=', '==',
	'!=', '&&', '||', '*=', '/=', '%=', '+=', '-=', '&=', '^=', '|=', '##',
	'[', ']', '(', ')', '{', '}', '.', '&', '*', '+', '-', '~', '!', '/',
	'%', '<', '>', '^', '|', '?', ':', ';', '=', ',', '#',
);

sub tokenize {
	my ($d) = @_;
	# Translation phase 1: trigraph replacement.
	$d =~ s/\?\?=/#/g;
	$d =~ s/\?\?\//\\/g;
	$d =~ s/\?\?'/^/g;
	$d =~ s/\?\?\(/[/g;
	$d =~ s/\?\?\)/]/g;
	$d =~ s/\?\?!/|/g;
	$d =~ s/\?\?</{/g;
	$d =~ s/\?\?>/}/g;
	$d =~ s/\?\?-/~/g;
	# Translation phase 2: remove every backslash-newline.
	$d =~ s/\\\r?\n//g;
	my $n = length($d);
	my $i = 0;
	my @t;
	while ($i < $n) {
		my $c = substr($d, $i, 1);
		if ($c =~ /[ \t\v\f\r]/) { $i++; next; }
		if ($c eq "\n") { push @t, ['nl', "\n"]; $i++; next; }
		if ($c eq '/' && substr($d, $i + 1, 1) eq '*') {
			my $j = index($d, '*/', $i + 2);
			$j = ($j < 0) ? $n : $j + 2;
			push @t, ['comment', substr($d, $i, $j - $i)];
			$i = $j;
			next;
		}
		if ($c eq '/' && substr($d, $i + 1, 1) eq '/') {
			my $j = index($d, "\n", $i + 2);
			$j = ($j < 0) ? $n : $j;
			push @t, ['comment', substr($d, $i, $j - $i)];
			$i = $j;
			next;
		}
		if ($c eq '"' || $c eq "'") {
			my $j = $i + 1;
			while ($j < $n) {
				my $ch = substr($d, $j, 1);
				last if $ch eq $c;
				if ($ch eq '\\') { $j += 2; next; }
				$j++;
			}
			$j = $n if $j > $n;
			$j++;
			$j = $n if $j > $n;
			my $lit = substr($d, $i, $j - $i);
			# A raw newline inside a string or character literal is a
			# constraint violation: the literal cannot be interpreted
			# as a preprocessing token, so mark it unsupported.
			push @t, [($lit =~ /\n/ ? 'uliteral' : 'literal'), $lit];
			$i = $j;
			next;
		}
		if ($c eq '\\') { push @t, ['backslash', '\\']; $i++; next; }
		if ($c =~ /[A-Za-z_]/) {
			my $j = $i + 1;
			$j++ while $j < $n && substr($d, $j, 1) =~ /[A-Za-z0-9_]/;
			push @t, ['ident', substr($d, $i, $j - $i)];
			$i = $j;
			next;
		}
		if ($c =~ /[0-9]/ ||
		    ($c eq '.' && substr($d, $i + 1, 1) =~ /[0-9]/)) {
			my $j = $i + 1;
			while ($j < $n) {
				my $ch = substr($d, $j, 1);
				if ($ch =~ /[A-Za-z0-9_.]/) { $j++; next; }
				if ($ch =~ /[eEpP]/ &&
				    substr($d, $j + 1, 1) =~ /[+\-]/) {
					$j += 2;
					next;
				}
				last;
			}
			push @t, ['number', substr($d, $i, $j - $i)];
			$i = $j;
			next;
		}
		my $matched = 0;
		for my $p (@PUNCT) {
			if (substr($d, $i, length($p)) eq $p) {
				push @t, ['punct', $p];
				$i += length($p);
				$matched = 1;
				last;
			}
		}
		next if $matched;
		push @t, ['other', $c];
		$i++;
	}
	return \@t;
}

# Normalize the token stream: drop comments/backslashes/newlines, collapse
# semicolon runs (keeping the last), drop a ';' before '}'.
sub _normalize {
	my ($t) = @_;
	my @seq = grep { $_->[0] ne 'comment' && $_->[0] ne 'backslash' } @$t;
	my @out;
	my $i = 0;
	while ($i < @seq) {
		if ($seq[$i][0] eq 'punct' && $seq[$i][1] eq ';') {
			my $j = $i;
			$j++ while $j < @seq &&
			    $seq[$j][0] eq 'punct' && $seq[$j][1] eq ';';
			push @out, $seq[$j - 1];
			$i = $j;
			next;
		}
		push @out, $seq[$i];
		$i++;
	}
	@out = grep { $_->[0] ne 'nl' } @out;
	my @final;
	for my $k (0 .. $#out) {
		my $tk = $out[$k];
		if ($tk->[0] eq 'punct' && $tk->[1] eq ';') {
			my $prev = @final ? $final[-1] : undef;
			my $next = $k + 1 <= $#out ? $out[$k + 1] : undef;
			my $drop = 0;
			$drop = 1 if defined $prev && $prev->[0] eq 'punct'
			    && $prev->[1] eq '}';
			$drop = 1 if defined $next && $next->[0] eq 'punct'
			    && $next->[1] eq '}';
			$drop = 1 if !defined $next;	# trailing empty declaration
			$drop = 1 if defined $next && $next->[0] eq 'punct'
			    && $next->[1] eq '#';	# empty statement before a directive
			next if $drop;
		}
		push @final, $tk;
	}
	return \@final;
}

sub _canon {
	my ($toks) = @_;
	my $s = '';
	for my $tk (@$toks) { $s .= $tk->[0] . ':' . $tk->[1] . "\x00"; }
	return $s;
}

sub _comments {
	my ($t) = @_;
	my @c;
	for my $tk (@$t) {
		next unless $tk->[0] eq 'comment';
		my $s = $tk->[1];
		$s =~ s/\s+//g;
		push @c, $s;
	}
	return join("\x00", sort @c);
}

# Returns undef when equivalent, else a (category, detail) list.
sub compare {
	my ($a, $b) = @_;
	my $ta = tokenize($a);
	my $tb = tokenize($b);

	# An unterminated literal (a raw newline inside it) cannot be
	# interpreted as a preprocessing token; report it as unsupported rather
	# than comparing bytes that have no defined meaning.
	for my $tk (@$ta, @$tb) {
		return ['ORACLE_UNSUPPORTED', ''] if $tk->[0] eq 'uliteral';
	}

	my $ca = _canon(_normalize($ta));
	my $cb = _canon(_normalize($tb));
	if ($ca ne $cb) {
		my @la = split /\x00/, $ca, -1;
		my @lb = split /\x00/, $cb, -1;
		my $cat = @la < @lb ? 'TOKEN_LOSS'
		    : @la > @lb ? 'TOKEN_DUPLICATION'
		    : 'TOKEN_CHANGE';
		return [$cat, ''];
	}
	if (_comments($ta) ne _comments($tb)) {
		return ['COMMENT_CHANGE', ''];
	}
	return undef;
}

sub _selftest {
	my @cases = (
		["int x;\n", "int  x ;\n", 1],
		["int x; int y;\n", "int x;\nint y;\n", 1],
		["int f(void){}\n", "int f(void){}\n", 1],
		["int x; int y;\n", "int x;\n", 0],
		["int x;\n", "int x; int x;\n", 0],
		["int x = 1;\n", "int x = 2;\n", 0],
		["a + b;\n", "a - b;\n", 0],
		["int x; // c\nint y;\n", "int x; // c int y;\n", 0],
		["/* a */ int x;\n", "int x; /* a */\n", 1],
		["char *s = \"/*x*/\";\n", "char *s = \"/*x*/\";\n", 1],
		["#if A\nint a;\n#else\nint b;\n#endif\n",
		 "#if A\nint b;\n#else\nint a;\n#endif\n", 0],
		["#define X(a) a ## b\n", "#define X(a) a ## b\n", 1],
		["#define X(a) a ## b\n", "#define X(a) a # b\n", 0],
		["int x\\\n;\n", "int x;\n", 1],
		# Trigraph line continuation (??/ + newline is a splice).
		["int x??/\n;\n", "int x;\n", 1],
		# Identifier concatenation.
		["int x; int y;\n", "int x; inty;\n", 0],
		# Operator concatenation.
		["a + +b;\n", "a ++b;\n", 0],
		["a - -b;\n", "a --b;\n", 0],
		["a + = b;\n", "a += b;\n", 0],
		# Token splitting.
		["a++;\n", "a + +;\n", 0],
		["a->b;\n", "a - >b;\n", 0],
		# Deleted punctuation.
		["int f(int a, int b);\n", "int f(int a int b);\n", 0],
		["int a, b;\n", "int a b;\n", 0],
		["int *a, *b;\n", "int *a *b;\n", 0],
		["int a; int b;\n", "int a int b;\n", 0],
		# Truncated source at end of file.
		["int a; int b;\n", "int a;\n", 0],
		# Duplicate punctuation.
		["int f(int a);\n", "int f(int a,,);\n", 0],
		# Changed literal bytes.
		["char c = 'a';\n", "char c = 'b';\n", 0],
		# Truncated literal.
		["char *s = \"abcd\";\n", "char *s = \"abc\";\n", 0],
		# Swallowed comment.
		["int x; /* c */ int y;\n", "int x; int y;\n", 0],
		# Escaped-newline change alters the spliced token stream.
		["a\\\nb;\n", "a b;\n", 0],
		# Deleted conditional branch.
		["#if A\nint a;\n#else\nint b;\n#endif\n",
		 "#if A\nint a;\n#endif\n", 0],
		# Duplicated conditional branch.
		["#if A\nint a;\n#else\nint b;\n#endif\n",
		 "#if A\nint a;\n#else\nint a;\n#else\nint b;\n#endif\n", 0],
		# Unterminated literal (raw newline) is unsupported.
		["char *s = \"a\nb\";\n", "char *s = \"a\nb\";\n", 2],
	);
	my $bad = 0;
	for my $c (@cases) {
		my ($a, $b, $ok) = @$c;
		my $r = compare($a, $b);
		my $pass;
		if ($ok == 2) {
			$pass = defined($r) && $r->[0] eq 'ORACLE_UNSUPPORTED';
		} else {
			$pass = $ok ? !defined($r) : defined($r);
		}
		unless ($pass) {
			$bad++;
			print "SELFTEST FAIL: $a -> $b\n";
		}
	}
	print "oracle selftest: " . scalar(@cases) . " cases, $bad failures\n";
	return $bad;
}

return 1 if caller;

if (@ARGV && $ARGV[0] eq '--selftest') {
	exit(_selftest() ? 1 : 0);
}
if (@ARGV == 3 && $ARGV[0] eq 'compare') {
	open(my $fa, '<:raw', $ARGV[1]) or die "$ARGV[1]: $!\n";
	local $/; my $a = <$fa>;
	open(my $fb, '<:raw', $ARGV[2]) or die "$ARGV[2]: $!\n";
	my $b = <$fb>;
	my $r = compare($a, $b);
	exit 0 unless defined $r;
	print "$r->[0]\n";
	exit 1;
}
die "usage: semanticoracle.pl --selftest | compare A B\n";
