# Brainfunk: The less ugly rewrite version

## Neo_Chen's Brainfuck Toolkit

![Brainfunk Logo](https://gitlab.com/Neo_Chen/Brainfunk/raw/master/Logo/Logo256px.png "Yes, this is the logo")

# Build Instruction
```
$ make
```
# Modes

bf: interpretes Brainfuck program

bfc: converts Brainfuck program to C

bit: prints out bitcode

# Different types of "code"

* text code (unprocessed plain Brainfuck)
* bitcode (internal data structure)
* bincode (processed format for virtual CPU execution)

# Usage
```
# Run Brainfuck program:
$ ./brainfunk -f <source file>

# Translate Brainfuck program to optimized C source:
$ ./brainfuck -m bfc -f <source file> [-o <output file>]
```
# Credits

* [brainfuck optimization strategies](http://calmerthanyouare.org/2015/01/07/optimizing-brainfuck.html) by Mats Linander
