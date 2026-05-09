#!/usr/bin/env perl
use strict;

my $var = shift // 'data';
my $hex = `od -An -t x1 -v`;

print "static const char $var\[\] = {\n";

for my $byte (split /\s+/, $hex) {
 next if $byte eq '';
 print "\t0x$byte,\n";
}

print "\t0x00,\n";
print "};\n";
