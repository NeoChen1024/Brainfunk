# Brainfunk
## Neo_Chen's Simple Brainfuck Interpreter
### Passed all the tests
Brainfunk is a simple speed-orientated interpreter, it doesn't have any fancy things like Byte Code (currently),
and dynamic memory allocation.
And VisualBrainfunk is just like Brainfunk, but with ncurses based UI and Visualization.
It has some simple array bounds checking, so normally it won't coredumps.

## Usage:
	brainfunk [-h] [-f file] [-c code] [-d]
	visualbrainfunk [-h] [-f file] [-c code] [-d] [-t msec]

* -d is for debugging, super verbose
* -f is the file to read, "-" for reading from stdin
* -c is run code directly from the argument
* -s sets code, stack, and memory size
* -t is delay between instructions, in millisecond
