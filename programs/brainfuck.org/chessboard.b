[chessboard.b -- output ASCII art chessboard from FEN input
(c) 2025 Daniel B. Cristofani
http://brainfuck.org/

This program is licensed under a Creative Commons Attribution-ShareAlike 4.0
International License (http://creativecommons.org/licenses/by-sa/4.0/).

This program expects a chess position specified in Forysth-Edwards Notation.
It translates that into an ASCII art depiction of that board state, using
ASCII art chess pieces made by DMC (dmc75287@gmx.us) for a program (at
https://pastebin.com/u/bf17/1/kM751Hqz) that shows the initial board state.

This one was fairly straightforward and also very difficult to get under 1024.
Many little tweaks, fiddly pointer movement. I'm sure this could be improved.

We read and output one row at a time. Basic data layout is:
m f b | \ F / B x 0 l p f p f p f p f p f p f p f p f 0 0 0 ....
At left we have three flags. m is -1 if we still have more data to read
(i.e. the row we're now working with is not the last one). f is dual-purpose:
it may mean we don't need to flip the background color this time (at the very
start of each row), or it may mean the current piece is black (fill flag).
b means that the current background color is black.

After that we have five semi-constant values used for ASCII output. | / and \
are close to most values we need to output; F is for fill value (piece color)
and B is for background color.

x is usually 0, but when we're about to do output it's where we put the
combined code for piece and line (1-4) within a piece which we'll use to
select what to output. l is the line counter (counting from 4 down to 1)
which also acts as a flag meaning we do have a row to output.

Then the following batch of f and p are fill values (white=1, black=2) and
piece codes (1-7 for empty/bknpqr).

I process input by first converting to binary with a binary counter, then
working with the bits. This is some of the fiddlier and dirtier part.

The arrangement of outer loops was another wrinkle. I need to output the
horizontal row-separator bar at start and end; but the start one wants to be
AFTER reading the input, so the input doesn't interrupt the board. What I
ended up doing was, if m is still nonzero, then read input, and set l=4 as a
secondary flag meaning we need to output this input, and update m to say
whether that's the last row (based on whether we ended with a space or slash).
Then, unconditionally, we output the horizontal bar; and then check the l flag
and use it as a line counter if we have an actual row to output. So the last
time through the outer loop, it skips both main sub-loops (input and output)
and only outputs the bar. And skipping the output loop also means it doesn't
move back to a nonzero for the end of the main loop, so it terminates.

Notice that my case statement feeds different fragments with identical ASCII
to the same case, to avoid duplication. Also note that the code for the top of
the pawn and for the bottom of all pieces need to consult flags internally.]

->>++++>-->++>->--[++++[>+++++++++++++++<-]>++<<]>>++[ build constants
  [<<]+<[ if need to read more
    >[>>>]++++[ while more chars need reading this row
      >>,[[>]+<[-<]>]>>>>>>>[ letter
        -<+[<<<<<<+>>>>>>-]<[<<<++<+>>>>-]<[-]<[<+>-]<[<++>-]<[<<+>>-]>
      ]<[
        -<[++++[<[<++>-]>-[<+>-]<]<[--[>+<-]+>]->] number
        <+[-[[-<]<<[<]<->]+[-<+]>[>>]]   row terminator 
      ]<
    ]
  ]>[>>]++++++++[<<<----.++.......++>>+>-]<<<----.++++>>++.[-]>> output bar  
  [if row to output
    [while line to output
      [>]<[>[>>+<<-]<<]
      >>>>[for each space
        <<<[<]<<<<<<.<<-[ if we need to flip background
          +>[->>>>>++++[>+<--]] flip black to white
          +>>[>>>---[>++<-]>>]<[<+>-]<[<] or white to black
        ]>>[>]<.>>>[>]>>[<<+[<]<-[>>]>-] copy piece code
        >-[-<<+[<]<<[<<]+>>>+++[>>]]<<+[<] set fill and fill flag
        <+[ not an empty space
          >>-[<+>-]+<[<++++++>>+<-]< combine line with piece code
          [------[+++[+[+[----[--[-[-[+++++[------[-[-[-[-[-[-[-[[+] select
          <<-------.<<<<+<[>->>...[>]]>[->>>+++...--->>>>]<<<+.++++++>>> 4bknpqr
          ]<[.<.<..>>.>]> 1b (top of bishop; l value of 4)
          ]<[.<<.>----.++++.>.>]> 1k
          ]<[.<<+++.--->---.+++.<<<.[>]]> 1n
          ]<[..<<<<<[[>]<.>]>[>>+++.--->>>>]<<..>]> 1p
          ]<[.<<.<<.>>>.>.>]> 1q
          ]<[<<-.++++.<<.>>.--.->>>]> 1r
          ]<[.<<.<.>>.>.>]> 2b
          ]]<[.<<<<-.>.<++.->>>>.>]> 23k
          ]<[<.<+++.<.>>------.++++++<---.>>>]> 2n
          ]]]]<[.<-------.<<.>>+.++++++>.>]> 2pq3bq
          ]<[.<.<<.[>.>]]> 3p
          ]<[.<.<<.>..>>>]> 3n
          ]]<[.<<-.<.>++.->>.>]> 23r
        ]<[.....>] empty
        <.<<<<<<[>>>---<<<-]>>[>>]>[>]>> clear fill
      ]<<<<[<<<]>>.[>>]++++++++++.[-]>- output | and linefeed
    ]+[>]<[[-]<]< wipe line data
  ]<
]
