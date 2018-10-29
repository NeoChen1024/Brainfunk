# Brainfunk
## Neo_Chen's Brainfuck Toolkit
### Passed all the tests
Brainfunk is a simple speed-orientated interpreter, it
doesn't have any fancy things like Byte Code (currently),
and dynamic memory allocation. VisualBrainfunk is just
like Brainfunk, but with ncurses based UI and Visualization.
Bitfunk is basically Brainfunk with bytecode and deduplication optimize.

They all have some simple array bounds checking, so normally it won't coredumps.

## Usage:
	brainfunk [-h] [-f file] [-b bitcode] [-c code] [-d]
	bitfunk [-h] [-f file] [-b bitcode] [-c code] [-d]
	visualbrainfunk [-h] [-f file] [-b bitcode] [-c code] [-d] [-t msec]
	visualbitfunk [-h] [-f file] [-b bitcode] [-c code] [-d] [-t msec]
	bf2bitcode [-h] [-f infile] [-o outfile] [-c code]
	bf2c [-h] [-f infile] [-b bitcode] [-o outfile] [-c code]

* -d is for debugging, super verbose
* -f is the file to read, "-" for reading from stdin
* -c is run code directly from the argument
* -s sets code, stack, memory, and bitcode size
* -t is delay between instructions, in millisecond


## bf2c Usage:
Use bf2c to convert Brainfuck code to C, to compile it, add libstdbfc.h to compiler's search path (-I\<path\>)
