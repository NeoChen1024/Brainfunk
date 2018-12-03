/* =================================== *
 * Brainfunk -- A Brainf**k Toolkit    *
 * Neo_Chen			       *
 * =================================== */

/* =================================== *\
||    Simpified version for Arduino    ||
\* =================================== */

#define TRUE 1
#define FALSE 0

typedef uint8_t memory_t;
typedef unsigned int stack_type;
typedef char code_t;
typedef unsigned int ptr_t;

#define MEMSIZE 512
#define CODESIZE 512
#define STACKSIZE 16

code_t code[CODESIZE];
memory_t memory[MEMSIZE];
stack_type stack[STACKSIZE];
ptr_t code_ptr=0;
ptr_t ptr=0;
ptr_t stack_ptr=0;
unsigned long int time=0;

void exit(int a)
{
	while(a);
}

void panic(char *msg)
{
	Serial.write('\n');
	Serial.println(msg);
	exit(2);
}

/* Read Code */
void read_code(void)
{
	ptr_t i=0;
	char c=0;
	while((c=Serial.read()) != '#') /* Use '#' to replace EOF */
	{
		/* Echo back code (Arduino seems don't have other low-level I/O function) */
		Serial.write(c);
		while(Serial.available() < 1); /* Wait until there's data to read */
		if(i >= CODESIZE)
			panic("?CODE");
		code[i++]=(char)c;
	}
	code[i]='#';
}

void push(stack_type *stack, unsigned int *ptr, stack_type content)
{
	if(*ptr >= STACKSIZE)
		panic("?>STACK");
	stack[++(*ptr)]=content;
}

stack_type pop(stack_type *stack, unsigned int *ptr)
{
	if(*ptr >= STACKSIZE)
		panic("?<STACK");
	return stack[(*ptr)--];
}

void jump_to_next_matching(void)
{
	int nest_level=0;
	while(code[code_ptr] != '\0')
	{
		if(code[code_ptr] == '[')
			++nest_level;
		else if(code[code_ptr] == ']')
			--nest_level;
		else if(nest_level == 0)
			return;
		code_ptr++;
	}
}

void interprete(code_t c)
{
	switch(c)
	{
		case '+':
			memory[ptr]++;
			++code_ptr;
			break;
		case '-':
			memory[ptr]--;
			++code_ptr;
			break;
		case '>':
			ptr++;
			if(ptr >= MEMSIZE)
				panic("?>MEM");
			++code_ptr;
			break;
		case '<':
			ptr--;
			if(ptr >= MEMSIZE)
				panic("?<MEM");
			++code_ptr;
			break;
		case '[':
			if(memory[ptr] == 0)
			{
				jump_to_next_matching(); /* Skip everything until reached matching "]" */
			}
			else
			{
				push(stack, &stack_ptr, code_ptr + 1); /* Push next PC */
				code_ptr++;
			}
			break;
		case ']':
			if(memory[ptr] != 0) /* if not equals to 0 */
			{
				code_ptr=stack[stack_ptr]; /* Peek */
			}
			else
			{
				code_ptr++;
				stack_ptr--; /* Drop */
			}
			break;
		case ',':
			memory[ptr]=(memory_t)Serial.read();
			++code_ptr;
			break;
		case '.':
			Serial.write(memory[ptr]);
			fflush(NULL);
			++code_ptr;
			break;
		case '#':
			Serial.println("?DONE");
			time = millis();
			Serial.print("TIME=");
			Serial.println(time);
			exit(1);
			break;
		default:
			++code_ptr;
			break;
	}
}

void setup() {
	Serial.begin(9600);
	Serial.println("\n\\");
	read_code();
	Serial.println("\n");
	Serial.println("?LOADED");
}

void loop() {
	while(TRUE)
		interprete(code[code_ptr]);
}
