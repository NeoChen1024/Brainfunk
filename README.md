# Brainfunk
## Neo_Chen's Simple Brainfuck Interpreter
### Passed all the tests
This is a simple speed-orientated interpreter, it don't have any fancy things like Byte Code (currently),
and dynamic memory allocation.
It has some simple array bounds checking, so normally it won't coredumps.

## Usage:
	brainfunk [-h] [-f file] [-c code] [-d]
* -d is for debugging, super verbose
* -f is the file to read, "-" for reading from stdin
* -c is run code directly from the argument
