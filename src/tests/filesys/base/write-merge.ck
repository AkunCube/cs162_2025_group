# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(write-merge) begin
(write-merge) create "writemerge"
(write-merge) open "writemerge"
(write-merge) write "writemerge" with 'a' for 131072 bytes
(write-merge) seek "writemerge" to 0
(write-merge) read "writemerge" with 'a' for 131072 bytes
(write-merge) close "writemerge"
(write-merge) end
EOF
pass;
