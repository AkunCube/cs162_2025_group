# -*- perl -*-
use strict;
use warnings;
use tests::tests;
check_expected (IGNORE_EXIT_CODES => 1, [<<'EOF']);
(write-full) begin
(write-full) create "writefull"
(write-full) open "writefull"
(write-full) write "writefull" for 131072 bytes
(write-full) close "writefull"
(write-full) read count is far less than write count
(write-full) end
EOF
pass;
