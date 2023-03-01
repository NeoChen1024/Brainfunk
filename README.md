# Brainfunk: Yet Another Brainfuck Interpreter

![Brainfunk Logo](https://gitlab.com/Neo_Chen/Brainfunk/raw/master/Logo/Logo256px.png "Yes, this is the logo")

# Build Instruction

```shell
$ make
```

# Features

* RLE optimizations for `+-` & `><`
* Various common loop structure optimizations ("set to zero", "multiply with offset", and "find zero")
* Can convert brainfuck to C code for compilation
* Is faster than older C version

# Modes

bf: interpretes Brainfuck program

bfc: converts Brainfuck program to C

bit: prints out bitcode

# Usage

```
# Run Brainfuck program:
$ ./brainfunk -f <source file>

# Translate Brainfuck program to optimized C source:
$ ./brainfuck -m bfc -f <source file> [-o <output file>]
```

# CPU core

The ultimate Brainfuck CPU
![CPU core](https://github.com/NeoChen1024/Brainfunk/raw/master/CPU/CPU.png)

# Credits

* [brainfuck optimization strategies](http://calmerthanyouare.org/2015/01/07/optimizing-brainfuck.html) by Mats Linander
* [compile-time-regular-expressions](https://github.com/hanickadot/compile-time-regular-expressions) library by Hana Dusíková
