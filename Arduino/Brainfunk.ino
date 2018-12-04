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
#define CODESIZE 768
#define STACKSIZE 16

code_t code[CODESIZE];
memory_t memory[MEMSIZE];
stack_type stack[STACKSIZE];
ptr_t code_ptr=0;
ptr_t ptr=0;
ptr_t stack_ptr=0;
unsigned long int starttime=0;

void (*reset)(void) = 0x0000;

void exit(int a)
{
	digitalWrite(LED_BUILTIN, LOW);
	Serial.flush();
	reset();
}

char readchar(void)
{
	char c=0;
	while(Serial.available() < 1); /* Wait until there's data to read */
	c = Serial.read();
	Serial.print(c);
	if(c == '\r')
		Serial.print("\n");
	return c;
}

void writechar(char c)
{
	if(c == '\n')
		Serial.write("\r\n");
	else
		Serial.write(c);
}

void panic(char *msg)
{
	Serial.write("\r\n");
	Serial.println(msg);
	exit(2);
}

/* Read Code */
void read_code(void)
{
	ptr_t i=0;
	char c=0;
	while((c=readchar()) != '#') /* Use '#' to replace EOF */
	{
		if(i >= CODESIZE)
			panic("?CODE");
		if(c == '+' || c == '-' || c == '>' || c == '<' ||
			c == '[' || c == ']' || c == '.' || c == ',')
		code[i++]=(char)c;
	}
	code[i]='\0';
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
			memory[ptr]=(memory_t)readchar();
			++code_ptr;
			break;
		case '.':
			writechar(memory[ptr]);
			fflush(NULL);
			++code_ptr;
			break;
		case '\0':
			Serial.print("\r\n?DONE=");
			Serial.println(millis() - starttime);
			exit(1);
			break;
		default:
			++code_ptr;
			break;
	}
}

void setup() {
	Serial.begin(9600);
	Serial.print("\a\r\n\\\r\n");
	Serial.print("CODESIZE:\t0x");
	Serial.println(CODESIZE, HEX);
	Serial.print("MEMSIZE:\t0x");
	Serial.println(MEMSIZE, HEX);
	Serial.println("\r\n\READY");
	read_code();
	Serial.println("\r\n?LOADED");
	pinMode(LED_BUILTIN, OUTPUT);
	digitalWrite(LED_BUILTIN, HIGH);
	starttime = millis();
}

void loop() {
	while(TRUE) /* Ensure the loop won't get the overhead of function call */
		interprete(code[code_ptr]);
}
