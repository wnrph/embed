/** @file      h2.c
 *  @brief     Compile for a small Forth
 *  @copyright Richard James Howe (2017)
 *  @license   MIT
 *
 * The project can be found at: https://github.com/howerj/forth-cpu */

/* ========================== Preamble: Types, Macros, Globals ============= */

#include "h2.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
extern int _fileno(FILE *stream);
#endif

static const char *log_levels[] =
{
#define X(ENUM, NAME) [ENUM] = NAME,
	X_MACRO_LOGGING
#undef X
};

log_level_e log_level = LOG_WARNING;

typedef struct {
	int error;
	int jmp_buf_valid;
	jmp_buf j;
} error_t;

/* ========================== Preamble: Types, Macros, Globals ============= */

/* ========================== Utilities ==================================== */

static int logger(log_level_e level, const char *func,
		const unsigned line, const char *fmt, ...)
{
	int r = 0;
	va_list ap;
       	assert(func);
       	assert(fmt);
	assert(level <= LOG_ALL_MESSAGES);
	if(level <= log_level) {
		fprintf(stderr, "[%s %u] %s: ", func, line, log_levels[level]);
		va_start(ap, fmt);
		r = vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
		fflush(stderr);
	}
	if(level == LOG_FATAL)
		exit(EXIT_FAILURE);
	return r;
}

static const char *reason(void)
{
	static const char *unknown = "unknown reason";
	const char *r;
	if(errno == 0)
		return unknown;
	r = strerror(errno);
	if(!r)
		return unknown;
	return r;
}

static void *allocate_or_die(size_t length)
{
	void *r;
	errno = 0;
	r = calloc(1, length);
	if(!r)
		fatal("allocation of size %zu failed: %s",
				length, reason());
	return r;
}

static FILE *fopen_or_die(const char *file, const char *mode)
{
	FILE *f = NULL;
	assert(file);
	assert(mode);
	errno = 0;
	f = fopen(file, mode);
	if(!f)
		fatal("failed to open file '%s' (mode %s): %s",
				file, mode, reason());
	return f;
}

static int indent(FILE *output, char c, unsigned i)
{
	assert(output);
	while(i--)
		if(fputc(c, output) != c)
			return -1;
	return 0;
}

static char *duplicate(const char *str)
{
	char *r;
	assert(str);
	errno = 0;
	r = malloc(strlen(str) + 1);
	if(!r)
		fatal("duplicate of '%s' failed: %s", str, reason());
	strcpy(r, str);
	return r;
}

static void ethrow(error_t *e)
{
	if(e && e->jmp_buf_valid) {
		e->jmp_buf_valid = 0;
		e->error = 1;
		longjmp(e->j, 1);
	}
	exit(EXIT_FAILURE);
}

static h2_t *h2_new(uint16_t start_address)
{
	h2_t *h = allocate_or_die(sizeof(h2_t));
	h->pc = start_address;
	for(uint16_t i = 0; i < start_address; i++)
		h->core[i] = OP_BRANCH | start_address;
	h->sp = VARIABLE_STACK_START;
	h->rp = RETURN_STACK_START;
	return h;
}

static void h2_free(h2_t *h)
{
	if(!h)
		return;
	memset(h, 0, sizeof(*h));
	free(h);
}

static int binary_memory_load(FILE *input, uint16_t *p, size_t length)
{
	assert(input);
	assert(p);
	for(size_t i = 0; i < length; i++) {
		errno = 0;
		int r1 = fgetc(input);
		int r2 = fgetc(input);
		if(r1 < 0 || r2 < 0) {
			debug("memory read failed: %s", strerror(errno));
			return -1;
		}
		p[i] = (((unsigned)r1 & 0xffu)) | (((unsigned)r2 & 0xffu) << 8u);
	}
	return 0;
}

static int binary_memory_save(FILE *output, uint16_t *p, size_t length)
{
	assert(output);
	assert(p);
	for(size_t i = 0; i < length; i++) {
		errno = 0;
		int r1 = fputc((p[i])&0xff,output);
		int r2 = fputc((p[i]>>8u)& 0xff, output);
		if(r1 < 0 || r2 < 0) {
			debug("memory write failed: %s", strerror(errno));
			return -1;
		}
	}
	return 0;
}

static int load(h2_t *h, const char *name)
{
	assert(h);
	assert(name);
	FILE *input = NULL;
	int r = 0;
	errno = 0;
	if((input = fopen(name, "rb"))) {
		r = binary_memory_load(input, h->core, MAX_MEMORY/2);
		fclose(input);
	} else {
		error("nvram file read (from %s) failed: %s", name, strerror(errno));
		r = -1;
	}
	return r;
}

static int save(h2_t *h, const char *name, size_t length)
{
	FILE *output = NULL;
	int r = 0;
	assert(h);
	assert(name);
	errno = 0;
	if((output = fopen(name, "wb"))) {
		r = binary_memory_save(output, h->core, length);
		fclose(output);
	} else {
		error("nvram file write (to %s) failed: %s", name, strerror(errno));
		r = -1;
	}
	return r;
}

#ifdef __unix__
#include <unistd.h>
#include <termios.h>
static int getch(void)
{
	struct termios oldattr, newattr;
	int ch;
	tcgetattr(STDIN_FILENO, &oldattr);
	newattr = oldattr;
	newattr.c_iflag &= ~(ICRNL);
	newattr.c_lflag &= ~(ICANON | ECHO);

	tcsetattr(STDIN_FILENO, TCSANOW, &newattr);
	ch = getchar();

	tcsetattr(STDIN_FILENO, TCSANOW, &oldattr);

	return ch;
}

static int putch(int c)
{
	int res = putchar(c);
	fflush(stdout);
	return res;
}
#else
#ifdef _WIN32

extern int getch(void);
extern int putch(int c);

#else
static int getch(void)
{
	return getchar();
}

static int putch(int c)
{
	return putchar(c);
}
#endif
#endif /** __unix__ **/

static int wrap_getch()
{
	int ch = getch();
	if(ch == EOF || ch == ESCAPE)
		exit(EXIT_SUCCESS);
	return ch;
}

/* ========================== Utilities ==================================== */

/* ========================== Symbol Table ================================= */

static const char *symbol_names[] =
{
	[SYMBOL_TYPE_LABEL]       = "label",
	[SYMBOL_TYPE_CALL]        = "call",
	[SYMBOL_TYPE_CONSTANT]    = "constant",
	[SYMBOL_TYPE_VARIABLE]    = "variable",
	NULL
};

static symbol_t *symbol_new(symbol_type_e type, const char *id, uint16_t value)
{
	symbol_t *s = allocate_or_die(sizeof(*s));
	assert(id);
	s->id = duplicate(id);
	s->value = value;
	s->type = type;
	return s;
}

static void symbol_free(symbol_t *s)
{
	if(!s)
		return;
	free(s->id);
	memset(s, 0, sizeof(*s));
	free(s);
}

static symbol_table_t *symbol_table_new(void)
{
	symbol_table_t *t = allocate_or_die(sizeof(*t));
	return t;
}

static void symbol_table_free(symbol_table_t *t)
{
	if(!t)
		return;
	for(size_t i = 0; i < t->length; i++)
		symbol_free(t->symbols[i]);
	free(t->symbols);
	memset(t, 0, sizeof(*t));
	free(t);
}

static symbol_t *symbol_table_lookup(symbol_table_t *t, const char *id)
{
	for(size_t i = 0; i < t->length; i++)
		if(!strcmp(t->symbols[i]->id, id))
			return t->symbols[i];
	return NULL;
}

static int symbol_table_add(symbol_table_t *t, symbol_type_e type, const char *id, uint16_t value, error_t *e, bool hidden)
{
	symbol_t *s = symbol_new(type, id, value);
	symbol_t **xs = NULL;
	assert(t);

	if(symbol_table_lookup(t, id)) {
		error("redefinition of symbol: %s", id);
		if(e)
			ethrow(e);
		else
			return -1;
	}
	s->hidden = hidden;

	t->length++;
	errno = 0;
	xs = realloc(t->symbols, sizeof(*t->symbols) * t->length);
	if(!xs)
		fatal("reallocate of size %zu failed: %s", t->length, reason());
	t->symbols = xs;
	t->symbols[t->length - 1] = s;
	return 0;
}

static int symbol_table_print(symbol_table_t *t, FILE *output)
{

	assert(t);
	for(size_t i = 0; i < t->length; i++) {
		symbol_t *s = t->symbols[i];
		char *visibility = s->hidden ? "hidden" : "visible";
		if(fprintf(output, "%s %s %"PRId16" %s\n", symbol_names[s->type], s->id, s->value, visibility) < 0)
			return -1;
	}
	return 0;
}

/* ========================== Symbol Table ================================= */

/* ========================== Simulation And Debugger ====================== */

static inline void dpush(h2_t *h, const uint16_t v)
{
	h->sp++;
	h->core[h->sp] = h->tos;
	h->tos = v;
}

static inline uint16_t dpop(h2_t *h)
{
	uint16_t r = h->tos;
	h->tos = h->core[h->sp--];
	return r;
}

static inline void rpush(h2_t *h, const uint16_t r)
{
	h->rp++;
	h->core[h->rp] = r;
}

static inline uint16_t stack_delta(uint16_t d)
{
	static const uint16_t i[4] = { 0x0000, 0x0001, 0xFFFE, 0xFFFF };
	return i[d];
}

int h2_run(h2_t *h)
{
	assert(h);

	for(;;) {
		uint16_t instruction,
			 literal,
			 address,
			 pc_plus_one;

		instruction = h->core[h->pc];

		literal = instruction & 0x7FFF;
		address = instruction & 0x1FFF; /* NB. also used for ALU OP */

		pc_plus_one = (h->pc + 1) % MAX_PROGRAM;

		/* decode / execute */
		if(IS_LITERAL(instruction)) {
			dpush(h, literal);
			h->pc = pc_plus_one;
		} else if (IS_ALU_OP(instruction)) {
			uint16_t rd  = stack_delta(RSTACK(instruction));
			uint16_t dd  = stack_delta(DSTACK(instruction));
			uint16_t nos = h->core[h->sp];
			uint16_t tos = h->tos;
			uint16_t npc = pc_plus_one;

			if(instruction & R_TO_PC)
				npc = h->core[h->rp] >> 1;

			switch(ALU_OP(instruction)) {
			case ALU_OP_T:        /* tos = tos; */                         break;
			case ALU_OP_N:           tos = nos;                            break;
			case ALU_OP_T_PLUS_N:    tos += nos;                           break;
			case ALU_OP_T_AND_N:     tos &= nos;                           break;
			case ALU_OP_T_OR_N:      tos |= nos;                           break;
			case ALU_OP_T_XOR_N:     tos ^= nos;                           break;
			case ALU_OP_T_INVERT:    tos = ~tos;                           break;
			case ALU_OP_T_EQUAL_N:   tos = -(tos == nos);                  break;
			case ALU_OP_N_LESS_T:    tos = -((int16_t)nos < (int16_t)tos); break;
			case ALU_OP_N_RSHIFT_T:  tos = nos >> tos;                     break;
			case ALU_OP_T_DECREMENT: tos--;                                break;
			case ALU_OP_R:           tos = h->core[h->rp];                 break;
			case ALU_OP_T_LOAD:      tos = h->core[(h->tos >> 1)];         break;
			case ALU_OP_N_LSHIFT_T:  tos = nos << tos;                     break;
			case ALU_OP_DEPTH:       tos = h->sp - VARIABLE_STACK_START;   break;
			case ALU_OP_N_ULESS_T:   tos = -(nos < tos);                   break;
			case ALU_OP_RDEPTH:      tos = h->rp - RETURN_STACK_START;     break;
			case ALU_OP_T_EQUAL_0:   tos = -(tos == 0);                    break;
			case ALU_OP_TX:          putch(tos); tos = nos;                break;
			case ALU_OP_RX:          tos = wrap_getch();                   break;
			case ALU_OP_SAVE:        save(h, FORTH_BLOCK, MAX_MEMORY/2);   break;
			case ALU_OP_BYE:         return tos;
			}

			h->sp += dd;
			h->rp += rd;

			if(instruction & T_TO_R)
				h->core[h->rp] = h->tos;

			if(instruction & T_TO_N)
				h->core[h->sp] = h->tos;

			if(instruction & N_TO_ADDR_T)
				h->core[(h->tos >> 1)] = nos;

			h->tos = tos;
			h->pc  = npc;
		} else if (IS_CALL(instruction)) {
			rpush(h, pc_plus_one << 1);
			h->pc = address;
		} else if (IS_0BRANCH(instruction)) {
			if(!dpop(h))
				h->pc = address % MAX_PROGRAM;
			else
				h->pc = pc_plus_one;
		} else /* if (IS_BRANCH(instruction)) */ {
			h->pc = address;
		}
	}
	return 0;
}

/* ========================== Simulation And Debugger ====================== */

/* ========================== Assembler ==================================== */
/* This section is the most complex, it implements a lexer, parser and code
 * compiler for a simple pseudo Forth like language, whilst it looks like
 * Forth it is not Forth. */

#define MAX_ID_LENGTH (256u)

/**@warning The ordering of the following enumerations matters a lot */
typedef enum {
	LEX_LITERAL,
	LEX_IDENTIFIER,
	LEX_LABEL,
	LEX_STRING,

	LEX_CONSTANT, /* start of named tokens */
	LEX_CALL,
	LEX_BRANCH,
	LEX_0BRANCH,
	LEX_BEGIN,
	LEX_WHILE,
	LEX_REPEAT,
	LEX_AGAIN,
	LEX_UNTIL,
	LEX_FOR,
	LEX_AFT,
	LEX_NEXT,
	LEX_IF,
	LEX_ELSE,
	LEX_THEN,
	LEX_DEFINE,
	LEX_ENDDEFINE,
	LEX_CHAR,
	LEX_VARIABLE,
	LEX_LOCATION,
	LEX_IMMEDIATE,
	LEX_HIDDEN,
	LEX_INLINE,
	LEX_QUOTE,

	LEX_PWD,
	LEX_SET,
	LEX_PC,
	LEX_MODE,
	LEX_ALLOCATE,
	LEX_BUILT_IN,

	/* start of instructions */
#define X(NAME, STRING, DEFINE, INSTRUCTION) LEX_ ## NAME,
	X_MACRO_INSTRUCTIONS
#undef X
	/* end of named tokens and instructions */

	LEX_ERROR, /* error token: this needs to be after the named tokens */

	LEX_EOI = EOF
} token_e;

static const char *keywords[] =
{
	[LEX_LITERAL]    = "literal",
	[LEX_IDENTIFIER] = "identifier",
	[LEX_LABEL]      = "label",
	[LEX_STRING]     = "string",
	[LEX_CONSTANT]   = "constant",
	[LEX_CALL]       = "call",
	[LEX_BRANCH]     = "branch",
	[LEX_0BRANCH]    = "0branch",
	[LEX_BEGIN]      = "begin",
	[LEX_WHILE]      = "while",
	[LEX_REPEAT]     = "repeat",
	[LEX_AGAIN]      = "again",
	[LEX_UNTIL]      = "until",
	[LEX_FOR]        = "for",
	[LEX_AFT]        = "aft",
	[LEX_NEXT]       = "next",
	[LEX_IF]         = "if",
	[LEX_ELSE]       = "else",
	[LEX_THEN]       = "then",
	[LEX_DEFINE]     = ":",
	[LEX_ENDDEFINE]  = ";",
	[LEX_CHAR]       = "[char]",
	[LEX_VARIABLE]   = "variable",
	[LEX_LOCATION]   = "location",
	[LEX_IMMEDIATE]  = "immediate",
	[LEX_HIDDEN]     = "hidden",
	[LEX_INLINE]     = "inline",
	[LEX_QUOTE]      = "'",
	[LEX_PWD]        = ".pwd",
	[LEX_SET]        = ".set",
	[LEX_PC]         = ".pc",
	[LEX_MODE]       = ".mode",
	[LEX_ALLOCATE]   = ".allocate",
	[LEX_BUILT_IN]   = ".built-in",

	/* start of instructions */
#define X(NAME, STRING, DEFINE, INSTRUCTION) [ LEX_ ## NAME ] = STRING,
	X_MACRO_INSTRUCTIONS
#undef X
	/* end of named tokens and instructions */

	[LEX_ERROR]      =  NULL,
	NULL
};

typedef struct {
	union {
		char *id;
		uint16_t number;
	} p;
	unsigned location;
	unsigned line;
	token_e type;
} token_t;

typedef struct {
	error_t error;
	FILE *input;
	unsigned line;
	int c;
	char id[MAX_ID_LENGTH];
	token_t *token;
	token_t *accepted;
	bool in_definition;
} lexer_t;

/********* LEXER *********/

/**@note it would be possible to add a very small amount of state to the
 * lexer, so when keywords like 'hex' and 'decimal' are encountered, the
 * base is changed. */

static token_t *token_new(token_e type, unsigned line)
{
	token_t *r = allocate_or_die(sizeof(*r));
	r->type = type;
	r->line = line;
	return r;
}

void token_free(token_t *t)
{
	if(!t)
		return;
	if(t->type == LEX_IDENTIFIER || t->type == LEX_STRING || t->type == LEX_LABEL)
		free(t->p.id);
	memset(t, 0, sizeof(*t));
	free(t);
}

static int next_char(lexer_t *l)
{
	assert(l);
	return fgetc(l->input);
}

static int unget_char(lexer_t *l, int c)
{
	assert(l);
	return ungetc(c, l->input);
}

static lexer_t* lexer_new(FILE *input)
{
	lexer_t *l = allocate_or_die(sizeof(lexer_t));
	l->input = input;
	return l;
}

static void lexer_free(lexer_t *l)
{
	assert(l);
	token_free(l->token);
	memset(l, 0, sizeof(*l));
	free(l);
}

static int token_print(token_t *t, FILE *output, unsigned depth)
{
	token_e type;
	int r = 0;
	if(!t)
		return 0;
	indent(output, ' ', depth);
	type = t->type;
	if(type == LEX_LITERAL) {
		r = fprintf(output, "number: %"PRId16, t->p.number);
	} else if(type == LEX_LABEL) {
		r = fprintf(output, "label: %s", t->p.id);
	} else if(type == LEX_IDENTIFIER) {
		r = fprintf(output, "id: %s", t->p.id);
	} else if(type == LEX_ERROR) {
		r = fputs("error", output);
	} else if(type == LEX_EOI) {
		r = fputs("EOI", output);
	} else {
		r = fprintf(output, "keyword: %s", keywords[type]);
	}
	return r < 0 ? -1 : 0;
}

static int _syntax_error(lexer_t *l,
		const char *func, unsigned line, const char *fmt, ...)
{
	va_list ap;
	assert(l);
	assert(func);
	assert(fmt);
	fprintf(stderr, "%s:%u\n", func, line);
	fprintf(stderr, "  syntax error on line %u of input\n", l->line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	token_print(l->token, stderr, 2);
	fputc('\n', stderr);
	ethrow(&l->error);
	return 0;
}

#define syntax_error(LEXER, ...) _syntax_error(LEXER, __func__, __LINE__, ## __VA_ARGS__)

static uint16_t map_char_to_number(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if(c >= 'a' && c <= 'z')
		return c + 10 - 'a';
	fatal("invalid numeric character: %c", c);
	return 0;
}

static bool numeric(int c, int base)
{
	assert(base == 10 || base == 16);
	if(base == 10)
		return isdigit(c);
	return isxdigit(c);
}

static int number(char *s, uint16_t *o, size_t length)
{
	size_t i = 0, start = 0;
	uint32_t out = 0;
	int base = 10;
	bool negate = false;
	assert(o);
	if(s[i] == '\0')
		return 0;

	if(s[i] == '-') {
		if(s[i+1] == '\0')
			return 0;
		negate = true;
		start = ++i;
	}

	if(s[i] == '$') {
		base = 16;
		if(s[i+1] == '\0')
			return 0;
		start = i + 1;
	}

	for(i = start; i < length; i++)
		if(!numeric(s[i], base))
			return 0;

	for(i = start; i < length; i++)
		out = out * base + map_char_to_number(s[i]);

	*o = negate ? out * -1 : out;
	return 1;
}

static void lexer(lexer_t *l)
{
	size_t i;
	int ch;
	token_e sym;
	uint16_t lit = 0;
	assert(l);
	ch = next_char(l);
	l->token = token_new(LEX_ERROR, l->line);

again:
	switch(ch) {
	case '\n':
		l->line++;
	case ' ':
	case '\t':
	case '\r':
	case '\v':
		ch = next_char(l);
		goto again;
	case EOF:
		l->token->type = LEX_EOI;
		return;
	case '\\':
		for(; '\n' != (ch = next_char(l));)
			if(ch == EOF)
				syntax_error(l, "'\\' commented terminated by EOF");
		ch = next_char(l);
		l->line++;
		goto again;
	case '(':
		ch = next_char(l);
		if(!isspace(ch)) {
			unget_char(l, ch);
			ch = '(';
			goto graph;
		}
		for(; ')' != (ch = next_char(l));)
			if(ch == EOF)
				syntax_error(l, "'(' comment terminated by EOF");
			else if(ch == '\n')
				l->line++;
		ch = next_char(l);
		goto again;
	case '"':
		for(i = 0; '"' != (ch = next_char(l));) {
			if(ch == EOF)
				syntax_error(l, "string terminated by EOF");
			if(i >= MAX_ID_LENGTH - 1)
				syntax_error(l, "identifier too large: %s", l->id);
			l->id[i++] = ch;
		}
		l->id[i] = '\0';
		l->token->type = LEX_STRING;
		l->token->p.id = duplicate(l->id);
		ch = next_char(l);
		break;
	default:
		i = 0;
	graph:
		if(isgraph(ch)) {
			while(isgraph(ch)) {
				if(i >= MAX_ID_LENGTH - 1)
					syntax_error(l, "identifier too large: %s", l->id);
				l->id[i++] = ch;
				ch = next_char(l);
			}
			l->id[i] = '\0';
		} else {
			syntax_error(l, "invalid character: %c", ch);
		}

		if(number(l->id, &lit, i)) {
			l->token->type = LEX_LITERAL;
			l->token->p.number = lit;
			break;
		}

		for(sym = LEX_CONSTANT; sym != LEX_ERROR && keywords[sym] && strcmp(keywords[sym], l->id); sym++)
			/*do nothing*/;
		if(!keywords[sym]) {
			if(i > 1 && l->id[i - 1] == ':') {
				l->id[strlen(l->id) - 1] = '\0';
				l->token->type = LEX_LABEL;
			} else { /* IDENTIFIER */
				l->token->type = LEX_IDENTIFIER;
			}
			l->token->p.id = duplicate(l->id);
		} else {
			l->token->type = sym;

			if(sym == LEX_DEFINE) {
				if(l->in_definition)
					syntax_error(l, "Nested definitions are not allowed");
				l->in_definition = true;
			}
			if(sym == LEX_ENDDEFINE) {
				if(!(l->in_definition))
					syntax_error(l, "Use of ';' not terminating word definition");
				l->in_definition = false;
			}
		}
		break;
	}
	unget_char(l, ch);
}

/********* PARSER *********/

#define X_MACRO_PARSE\
	X(SYM_PROGRAM,             "program")\
	X(SYM_STATEMENTS,          "statements")\
	X(SYM_LABEL,               "label")\
	X(SYM_BRANCH,              "branch")\
	X(SYM_0BRANCH,             "0branch")\
	X(SYM_CALL,                "call")\
	X(SYM_CONSTANT,            "constant")\
	X(SYM_VARIABLE,            "variable")\
	X(SYM_LOCATION,            "location")\
	X(SYM_LITERAL,             "literal")\
	X(SYM_STRING,              "string")\
	X(SYM_INSTRUCTION,         "instruction")\
	X(SYM_BEGIN_UNTIL,         "begin...until")\
	X(SYM_BEGIN_AGAIN,         "begin...again")\
	X(SYM_BEGIN_WHILE_REPEAT,  "begin...while...repeat")\
	X(SYM_FOR_NEXT,            "for...next")\
	X(SYM_FOR_AFT_THEN_NEXT,   "for...aft...then...next")\
	X(SYM_IF1,                 "if1")\
	X(SYM_DEFINITION,          "definition")\
	X(SYM_CHAR,                "[char]")\
	X(SYM_QUOTE,               "'")\
	X(SYM_PWD,                 "pwd")\
	X(SYM_SET,                 "set")\
	X(SYM_PC,                  "pc")\
	X(SYM_BUILT_IN,            "built-in")\
	X(SYM_MODE,                "mode")\
	X(SYM_ALLOCATE,            "allocate")\
	X(SYM_CALL_DEFINITION,     "call-definition")

typedef enum {
#define X(ENUM, NAME) ENUM,
	X_MACRO_PARSE
#undef X
} parse_e;

static const char *names[] = {
#define X(ENUM, NAME) [ENUM] = NAME,
	X_MACRO_PARSE
#undef X
	NULL
};

typedef struct node_t  {
	parse_e type;
	size_t length;
	uint16_t bits; /*general use bits*/
	token_t *token, *value;
	struct node_t *o[];
} node_t;

static node_t *node_new(parse_e type, size_t size)
{
	node_t *r = allocate_or_die(sizeof(*r) + sizeof(r->o[0]) * size);
	if(log_level >= LOG_DEBUG)
		fprintf(stderr, "node> %s\n", names[type]);
	r->length = size;
	r->type = type;
	return r;
}

static node_t *node_grow(node_t *n)
{
	node_t *r = NULL;
	assert(n);
	errno = 0;
	r = realloc(n, sizeof(*n) + (sizeof(n->o[0]) * (n->length + 1)));
	if(!r)
		fatal("reallocate of size %zu failed: %s", n->length + 1, reason());
	r->o[r->length++] = 0;
	return r;
}

static void node_free(node_t *n)
{
	if(!n)
		return;
	for(unsigned i = 0; i < n->length; i++)
		node_free(n->o[i]);
	token_free(n->token);
	token_free(n->value);
	free(n);
}

static int accept(lexer_t *l, token_e sym)
{
	assert(l);
	if(sym == l->token->type) {
		token_free(l->accepted); /* free token owned by lexer */
		l->accepted = l->token;
		if(sym != LEX_EOI)
			lexer(l);
		return 1;
	}
	return 0;
}

static int accept_range(lexer_t *l, token_e low, token_e high)
{
	assert(l);
	assert(low <= high);
	for(token_e i = low; i <= high; i++)
		if(accept(l, i))
			return 1;
	return 0;
}

static void use(lexer_t *l, node_t *n)
{ /* move ownership of token from lexer to parse tree */
	assert(l);
	assert(n);
	if(n->token)
		n->value = l->accepted;
	else
		n->token = l->accepted;
	l->accepted = NULL;
}

static int token_enum_print(token_e sym, FILE *output)
{
	assert(output);
	assert(sym < LEX_ERROR);
	const char *s = keywords[sym];
	return fprintf(output, "%s(%u)", s ? s : "???", sym);
}

static void node_print(FILE *output, node_t *n, bool shallow, unsigned depth)
{
	if(!n)
		return;
	assert(output);
	indent(output, ' ', depth);
	fprintf(output, "node(%d): %s\n", n->type, names[n->type]);
	token_print(n->token, output, depth);
	if(n->token)
		fputc('\n', output);
	if(shallow)
		return;
	for(size_t i = 0; i < n->length; i++)
		node_print(output, n->o[i], shallow, depth+1);
}

static int _expect(lexer_t *l, token_e token, const char *file, const char *func, unsigned line)
{
	assert(l);
	assert(file);
	assert(func);
	if(accept(l, token))
		return 1;
	fprintf(stderr, "%s:%s:%u\n", file, func, line);
	fprintf(stderr, "  Syntax error: unexpected token\n  Got:          ");
	token_print(l->token, stderr, 0);
	fputs("  Expected:     ", stderr);
	token_enum_print(token, stderr);
	fprintf(stderr, "\n  On line: %u\n", l->line);
	ethrow(&l->error);
	return 0;
}

#define expect(L, TOKEN) _expect((L), (TOKEN), __FILE__, __func__, __LINE__)

/* for rules in the BNF tree defined entirely by their token */
static node_t *defined_by_token(lexer_t *l, parse_e type)
{
	node_t *r;
	assert(l);
	r = node_new(type, 0);
	use(l, r);
	return r;
}

typedef enum {
	DEFINE_HIDDEN    = 1 << 0,
	DEFINE_IMMEDIATE = 1 << 1,
	DEFINE_INLINE    = 1 << 2,
} define_type_e;

/** @note LEX_LOCATION handled by modifying return node in statement() */
static node_t *variable_or_constant(lexer_t *l, bool variable)
{
	node_t *r;
	assert(l);
	r = node_new(variable ? SYM_VARIABLE : SYM_CONSTANT, 1);
	expect(l, LEX_IDENTIFIER);
	use(l, r);
	if(accept(l, LEX_LITERAL)) {
		r->o[0] = defined_by_token(l, SYM_LITERAL);
	} else {
		expect(l, LEX_STRING);
		r->o[0] = defined_by_token(l, SYM_STRING);
	}
	if(accept(l, LEX_HIDDEN)) {
		if(r->bits & DEFINE_HIDDEN)
			syntax_error(l, "hidden bit already set on latest word definition");
		r->bits |= DEFINE_HIDDEN;
	}
	return r;
}

static node_t *jump(lexer_t *l, parse_e type)
{
	node_t *r;
	assert(l);
	r = node_new(type, 0);
	(void)(accept(l, LEX_LITERAL) || accept(l, LEX_STRING) || expect(l, LEX_IDENTIFIER));
	use(l, r);
	return r;
}

static node_t *statements(lexer_t *l);

static node_t *for_next(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_FOR_NEXT, 1);
	r->o[0] = statements(l);
	if(accept(l, LEX_AFT)) {
		r->type = SYM_FOR_AFT_THEN_NEXT;
		r = node_grow(r);
		r->o[1] = statements(l);
		r = node_grow(r);
		expect(l, LEX_THEN);
		r->o[2] = statements(l);
	}
	expect(l, LEX_NEXT);
	return r;
}

static node_t *begin(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_BEGIN_UNTIL, 1);
	r->o[0] = statements(l);
	if(accept(l, LEX_AGAIN)) {
		r->type = SYM_BEGIN_AGAIN;
	} else if(accept(l, LEX_WHILE)) {
		r->type = SYM_BEGIN_WHILE_REPEAT;
		r = node_grow(r);
		r->o[1] = statements(l);
		expect(l, LEX_REPEAT);
	} else {
		expect(l, LEX_UNTIL);
	}
	return r;
}

static node_t *if1(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_IF1, 2);
	r->o[0] = statements(l);
	if(accept(l, LEX_ELSE))
		r->o[1] = statements(l);
	expect(l, LEX_THEN);
	return r;
}

static node_t *define(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_DEFINITION, 1);
	if(accept(l, LEX_IDENTIFIER))
		;
	else
		expect(l, LEX_STRING);
	use(l, r);
	r->o[0] = statements(l);
	expect(l, LEX_ENDDEFINE);
again:
	if(accept(l, LEX_IMMEDIATE)) {
		if(r->bits & DEFINE_IMMEDIATE)
			syntax_error(l, "immediate bit already set on latest word definition");
		r->bits |= DEFINE_IMMEDIATE;
		goto again;
	}
	if(accept(l, LEX_HIDDEN)) {
		if(r->bits & DEFINE_HIDDEN)
			syntax_error(l, "hidden bit already set on latest word definition");
		r->bits |= DEFINE_HIDDEN;
		goto again;
	}
	if(accept(l, LEX_INLINE)) {
		if(r->bits & DEFINE_INLINE)
			syntax_error(l, "inline bit already set on latest word definition");
		r->bits |= DEFINE_INLINE;
		goto again;
	}
	return r;
}

static node_t *char_compile(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_CHAR, 0);
	expect(l, LEX_IDENTIFIER);
	use(l, r);
	if(strlen(r->token->p.id) > 1)
		syntax_error(l, "expected single character, got identifier: %s", r->token->p.id);
	return r;
}

static node_t *mode(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_MODE, 0);
	expect(l, LEX_LITERAL);
	use(l, r);
	return r;
}

static node_t *pc(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_PC, 0);
	if(!accept(l, LEX_LITERAL))
		expect(l, LEX_IDENTIFIER);
	use(l, r);
	return r;
}

static node_t *pwd(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_PWD, 0);
	if(!accept(l, LEX_LITERAL))
		expect(l, LEX_IDENTIFIER);
	use(l, r);
	return r;
}

static node_t *set(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_SET, 0);
	if(!accept(l, LEX_IDENTIFIER))
		expect(l, LEX_LITERAL);
	use(l, r);
	if(!accept(l, LEX_IDENTIFIER) && !accept(l, LEX_STRING))
		expect(l, LEX_LITERAL);
	use(l, r);
	return r;
}

static node_t *allocate(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_ALLOCATE, 0);
	if(!accept(l, LEX_IDENTIFIER))
		expect(l, LEX_LITERAL);
	use(l, r);
	return r;
}

static node_t *quote(lexer_t *l)
{
	node_t *r;
	assert(l);
	r = node_new(SYM_QUOTE, 0);
	if(!accept(l, LEX_IDENTIFIER))
		expect(l, LEX_STRING);
	use(l, r);
	return r;
}

static node_t *statements(lexer_t *l)
{
	node_t *r;
	size_t i = 0;
	assert(l);
	r = node_new(SYM_STATEMENTS, 2);
again:
	r = node_grow(r);
	if(accept(l, LEX_CALL)) {
		r->o[i++] = jump(l, SYM_CALL);
		goto again;
	} else if(accept(l, LEX_BRANCH)) {
		r->o[i++] = jump(l, SYM_BRANCH);
		goto again;
	} else if(accept(l, LEX_0BRANCH)) {
		r->o[i++] = jump(l, SYM_0BRANCH);
		goto again;
	} else if(accept(l, LEX_LITERAL)) {
		r->o[i++] = defined_by_token(l, SYM_LITERAL);
		goto again;
	} else if(accept(l, LEX_LABEL)) {
		r->o[i++] = defined_by_token(l, SYM_LABEL);
		goto again;
	} else if(accept(l, LEX_CONSTANT)) {
		r->o[i++] = variable_or_constant(l, false);
		goto again;
	} else if(accept(l, LEX_VARIABLE)) {
		r->o[i++] = variable_or_constant(l, true);
		goto again;
	} else if(accept(l, LEX_LOCATION)) {
		r->o[i]   = variable_or_constant(l, true);
		r->o[i++]->type = SYM_LOCATION;
		goto again;
	} else if(accept(l, LEX_IF)) {
		r->o[i++] = if1(l);
		goto again;
	} else if(accept(l, LEX_DEFINE)) {
		r->o[i++] = define(l);
		goto again;
	} else if(accept(l, LEX_CHAR)) {
		r->o[i++] = char_compile(l);
		goto again;
	} else if(accept(l, LEX_BEGIN)) {
		r->o[i++] = begin(l);
		goto again;
	} else if(accept(l, LEX_FOR)) {
		r->o[i++] = for_next(l);
		goto again;
	} else if(accept(l, LEX_QUOTE)) {
		r->o[i++] = quote(l);
		goto again;
	} else if(accept(l, LEX_IDENTIFIER)) {
		r->o[i++] = defined_by_token(l, SYM_CALL_DEFINITION);
		goto again;
	} else if(accept(l, LEX_PWD)) {
		r->o[i++] = pwd(l);
		goto again;
	} else if(accept(l, LEX_SET)) {
		r->o[i++] = set(l);
		goto again;
	} else if(accept(l, LEX_PC)) {
		r->o[i++] = pc(l);
		goto again;
	} else if(accept(l, LEX_MODE)) {
		r->o[i++] = mode(l);
		goto again;
	} else if(accept(l, LEX_ALLOCATE)) {
		r->o[i++] = allocate(l);
		goto again;
	} else if(accept(l, LEX_BUILT_IN)) {
		r->o[i++] = defined_by_token(l, SYM_BUILT_IN);
		goto again;
	/**@warning This is a token range from the first instruction to the
	 * last instruction */
	} else if(accept_range(l, LEX_DUP, LEX_RDROP)) {
		r->o[i++] = defined_by_token(l, SYM_INSTRUCTION);
		goto again;
	}
	return r;
}

static node_t *program(lexer_t *l) /* block ( "." | EOF ) */
{
	node_t *r;
	assert(l);
	r = node_new(SYM_PROGRAM, 1);
	lexer(l);
	r->o[0] = statements(l);
	expect(l, LEX_EOI);
	return r;
}

static node_t *parse(FILE *input)
{
	lexer_t *l;
	assert(input);
	l = lexer_new(input);
	l->error.jmp_buf_valid = 1;
	if(setjmp(l->error.j)) {
		lexer_free(l);
		return NULL;
	}
	node_t *n = program(l);
	lexer_free(l);
	return n;
}

/********* CODE ***********/

typedef enum {
	MODE_NORMAL              = 0 << 0,
	MODE_COMPILE_WORD_HEADER = 1 << 0,
	MODE_OPTIMIZATION_ON     = 1 << 1,
} assembler_mode_e;

typedef struct {
	bool in_definition;
	bool start_defined;
	bool built_in_words_defined;
	uint16_t start;
	uint16_t mode;
	uint16_t pwd; /* previous word register */
	uint16_t fence; /* mark a boundary before which optimization cannot take place */
	symbol_t *do_r_minus_one;
	symbol_t *do_next;
	symbol_t *do_var;
	symbol_t *do_const;
} assembler_t;

static void update_fence(assembler_t *a, uint16_t pc)
{
	assert(a);
	a->fence = MAX(a->fence, pc);
}

static void generate(h2_t *h, assembler_t *a, uint16_t instruction)
{
	assert(h);
	assert(a);
	debug("%"PRIx16":\t%"PRIx16, h->pc, instruction);

	if(IS_CALL(instruction) || IS_LITERAL(instruction) || IS_0BRANCH(instruction) || IS_BRANCH(instruction))
		update_fence(a, h->pc);

	/** @note This implements two ad-hoc optimizations, both related to
	 * CODE_EXIT, they should be replaced by a generic peep hole optimizer */
	if(a->mode & MODE_OPTIMIZATION_ON && h->pc) {
		uint16_t previous = h->core[h->pc - 1];
		if(((h->pc - 1) > a->fence) && IS_ALU_OP(previous) && (instruction == CODE_EXIT)) {
			/* merge the CODE_EXIT instruction with the previous instruction if it is possible to do so */
			if(!(previous & R_TO_PC) && !(previous & MK_RSTACK(DELTA_N1))) {
				debug("optimization EXIT MERGE pc(%04"PRIx16 ") [%04"PRIx16 " -> %04"PRIx16"]", h->pc, previous, previous|instruction);
				previous |= instruction;
				h->core[h->pc - 1] = previous;
				update_fence(a, h->pc - 1);
				return;
			}
		} else if(h->pc > a->fence && IS_CALL(previous) && (instruction == CODE_EXIT)) {
			/* do not emit CODE_EXIT if last instruction in a word
			 * definition is a call, instead replace that call with
			 * a jump */
			debug("optimization TAIL CALL pc(%04"PRIx16 ") [%04"PRIx16 " -> %04"PRIx16"]", h->pc, previous, OP_BRANCH | (previous & 0x1FFF));
			h->core[h->pc - 1] = (OP_BRANCH | (previous & 0x1FFF));
			update_fence(a, h->pc - 1);
			return;
		}
	}

	h->core[h->pc++] = instruction;
}

static uint16_t here(h2_t *h, assembler_t *a)
{
	assert(h);
	assert(h->pc < MAX_PROGRAM);
	update_fence(a, h->pc);
	return h->pc;
}

static uint16_t hole(h2_t *h, assembler_t *a)
{
	assert(h);
	assert(h->pc < MAX_PROGRAM);
	here(h, a);
	return h->pc++;
}

static void fix(h2_t *h, uint16_t hole, uint16_t patch)
{
	assert(h);
	assert(hole < MAX_PROGRAM);
	h->core[hole] = patch;
}

#define assembly_error(ERROR, FMT, ...) do{ error(FMT, ##__VA_ARGS__); ethrow(e); }while(0)

static void generate_jump(h2_t *h, assembler_t *a, symbol_table_t *t, token_t *tok, parse_e type, error_t *e)
{
	uint16_t or = 0;
	uint16_t addr = 0;
	symbol_t *s;
	assert(h);
	assert(t);
	assert(a);

	if(tok->type == LEX_IDENTIFIER || tok->type == LEX_STRING) {
		s = symbol_table_lookup(t, tok->p.id);
		if(!s)
			assembly_error(e, "undefined symbol: %s", tok->p.id);
		addr = s->value;

		if(s->type == SYMBOL_TYPE_CALL && type != SYM_CALL)
			assembly_error(e, "cannot branch/0branch to call: %s", tok->p.id);

	} else if (tok->type == LEX_LITERAL) {
		addr = tok->p.number;
	} else {
		fatal("invalid jump target token type");
	}

	if(addr > MAX_PROGRAM)
		assembly_error(e, "invalid jump address: %"PRId16, addr);

	switch(type) {
	case SYM_BRANCH:  or = OP_BRANCH ; break;
	case SYM_0BRANCH: or = OP_0BRANCH; break;
	case SYM_CALL:    or = OP_CALL;    break;
	default:
		fatal("invalid call type: %u", type);
	}
	generate(h, a, or | addr);
}

static void generate_literal(h2_t *h, assembler_t *a, uint16_t number)
{
	if(number & OP_LITERAL) {
		number = ~number;
		generate(h, a, OP_LITERAL | number);
		generate(h, a, CODE_INVERT);
	} else {
		generate(h, a, OP_LITERAL | number);
	}
}

static uint16_t lexer_to_alu_op(token_e t)
{
	assert(t >= LEX_DUP && t <= LEX_RDROP);
	switch(t) {
#define X(NAME, STRING, DEFINE, INSTRUCTION) case LEX_ ## NAME : return CODE_ ## NAME ;
	X_MACRO_INSTRUCTIONS
#undef X
	default: fatal("invalid ALU operation: %u", t);
	}
	return 0;
}

static uint16_t literal_or_symbol_lookup(token_t *token, symbol_table_t *t, error_t *e)
{
	symbol_t *s = NULL;
	assert(token);
	assert(t);
	if(token->type == LEX_LITERAL)
		return token->p.number;

	assert(token->type == LEX_IDENTIFIER);

	if(!(s = symbol_table_lookup(t, token->p.id)))
		assembly_error(e, "symbol not found: %s", token->p.id);
	return s->value;
}

static uint16_t pack_16(const char lb, const char hb)
{
	return (((uint16_t)hb) << 8) | (uint16_t)lb;
}

static uint16_t pack_string(h2_t *h, assembler_t *a, const char *s, error_t *e)
{
	assert(h);
	assert(s);
	size_t l = strlen(s);
	size_t i = 0;
	uint16_t r = h->pc;
	if(l > 255)
		assembly_error(e, "string \"%s\" is too large (%zu > 255)", s, l);
	h->core[hole(h, a)] = pack_16(l, s[0]);
	for(i = 1; i < l; i += 2)
		h->core[hole(h, a)] = pack_16(s[i], s[i+1]);
	if(i < l)
		h->core[hole(h, a)] = pack_16(s[i], 0);
	here(h, a);
	return r;
}

static uint16_t symbol_special(h2_t *h, assembler_t *a, const char *id, error_t *e)
{
	static const char *special[] = {
		"$pc",
		"$pwd",
		NULL
	};

	enum special_e {
		SPECIAL_VARIABLE_PC,
		SPECIAL_VARIABLE_PWD
	};

	size_t i;
	assert(h);
	assert(id);
	assert(a);

	for(i = 0; special[i]; i++)
		if(!strcmp(id, special[i]))
			break;
	if(!special[i])
		assembly_error(e, "'%s' is not a symbol", id);

	switch(i) {
	case SPECIAL_VARIABLE_PC:   return h->pc << 1;
	case SPECIAL_VARIABLE_PWD:  return a->pwd; /**@note already as a character address */
	default: fatal("reached the unreachable: %zu", i);
	}

	return 0;
}

typedef struct {
	char *name;
	size_t len;
	bool inline_bit;
	bool hidden;
	bool compile;
	uint16_t code[32];
} built_in_words_t;

static built_in_words_t built_in_words[] = {
#define X(NAME, STRING, DEFINE, INSTRUCTION) \
	{\
		.name = STRING,\
		.compile = DEFINE,\
		.len = 1,\
		.inline_bit = true,\
		.hidden = false,\
		.code = { INSTRUCTION }\
	},
	X_MACRO_INSTRUCTIONS
#undef X
	/**@note We might want to compile these words, even if we are not
	 * compiling the other in-line-able, so the compiler can use them for
	 * variable declaration and for...next loops */
	{ .name = "doVar",   .compile = true, .inline_bit = false, .hidden = true, .len = 1, .code = {CODE_FROMR} },
	{ .name = "doConst", .compile = true, .inline_bit = false, .hidden = true, .len = 2, .code = {CODE_FROMR, CODE_LOAD} },
	{ .name = "r1-",     .compile = true, .inline_bit = false, .hidden = true, .len = 5, .code = {CODE_FROMR, CODE_FROMR, CODE_T_N1, CODE_TOR, CODE_TOR} },
	{ .name = NULL,      .compile = true, .inline_bit = false, .hidden = true, .len = 0, .code = {0} }
};

static void generate_loop_decrement(h2_t *h, assembler_t *a, symbol_table_t *t)
{
	a->do_r_minus_one = a->do_r_minus_one ? a->do_r_minus_one : symbol_table_lookup(t, "r1-");
	if(a->do_r_minus_one && a->mode & MODE_OPTIMIZATION_ON) {
		generate(h, a, OP_CALL | a->do_r_minus_one->value);
	} else {
		generate(h, a, CODE_FROMR);
		generate(h, a, CODE_T_N1);
		generate(h, a, CODE_TOR);
	}
}

static void assemble(h2_t *h, assembler_t *a, node_t *n, symbol_table_t *t, error_t *e)
{
	uint16_t hole1, hole2;
	assert(h);
	assert(t);
	assert(e);

	if(!n)
		return;

	if(h->pc > MAX_PROGRAM)
		assembly_error(e, "PC/Dictionary overflow: %"PRId16, h->pc);

	switch(n->type) {
	case SYM_PROGRAM:
		assemble(h, a, n->o[0], t, e);
		break;
	case SYM_STATEMENTS:
		for(size_t i = 0; i < n->length; i++)
			assemble(h, a, n->o[i], t, e);
		break;
	case SYM_LABEL:
		symbol_table_add(t, SYMBOL_TYPE_LABEL, n->token->p.id, here(h, a), e, false);
		break;
	case SYM_BRANCH:
	case SYM_0BRANCH:
	case SYM_CALL:
		generate_jump(h, a, t, n->token, n->type, e);
		break;
	case SYM_CONSTANT:
		if(a->mode & MODE_COMPILE_WORD_HEADER && a->built_in_words_defined && (!(n->bits & DEFINE_HIDDEN))) {
			a->do_const = a->do_const ? a->do_const : symbol_table_lookup(t, "doConst");
			assert(a->do_const);
			hole1 = hole(h, a);
			fix(h, hole1, a->pwd);
			a->pwd = hole1 << 1;
			pack_string(h, a, n->token->p.id, e);
			generate(h, a, OP_CALL | a->do_const->value);
			hole1 = hole(h, a);
			fix(h, hole1, n->o[0]->token->p.number);
		}
		symbol_table_add(t, SYMBOL_TYPE_CONSTANT, n->token->p.id, n->o[0]->token->p.number, e, false);
		break;
	case SYM_VARIABLE:
		if(a->mode & MODE_COMPILE_WORD_HEADER && a->built_in_words_defined && (!(n->bits & DEFINE_HIDDEN))) {
			a->do_var = a->do_var ? a->do_var : symbol_table_lookup(t, "doVar");
			assert(a->do_var);
			hole1 = hole(h, a);
			fix(h, hole1, a->pwd);
			a->pwd = hole1 << 1;
			pack_string(h, a, n->token->p.id, e);
			generate(h, a, OP_CALL | a->do_var->value);
		} else if (!(n->bits & DEFINE_HIDDEN)) {
			assembly_error(e, "variable used but doVar not defined, use location");
		}
		/* fall through */
	case SYM_LOCATION:
		here(h, a);

		if(n->o[0]->token->type == LEX_LITERAL) {
			hole1 = hole(h, a);
			fix(h, hole1, n->o[0]->token->p.number);
		} else {
			assert(n->o[0]->token->type == LEX_STRING);
			hole1 = pack_string(h, a, n->o[0]->token->p.id, e);
		}

		/**@note The lowest bit of the address for memory loads is
		 * discarded. */
		symbol_table_add(t, SYMBOL_TYPE_VARIABLE, n->token->p.id, hole1 << 1, e, n->type == SYM_LOCATION ? true : false);
		break;
	case SYM_QUOTE:
	{
		symbol_t *s = symbol_table_lookup(t, n->token->p.id);
		if(!s || (s->type != SYMBOL_TYPE_CALL && s->type != SYMBOL_TYPE_LABEL))
			assembly_error(e, "not a defined procedure: %s", n->token->p.id);
		generate_literal(h, a, s->value << 1);
		break;
	}
	case SYM_LITERAL:
		generate_literal(h, a, n->token->p.number);
		break;
	case SYM_INSTRUCTION:
		generate(h, a, lexer_to_alu_op(n->token->type));
		break;
	case SYM_BEGIN_AGAIN: /* fall through */
	case SYM_BEGIN_UNTIL:
		hole1 = here(h, a);
		assemble(h, a, n->o[0], t, e);
		generate(h, a, (n->type == SYM_BEGIN_AGAIN ? OP_BRANCH : OP_0BRANCH) | hole1);
		break;

	case SYM_FOR_NEXT:
	{
		symbol_t *s = a->do_next ? a->do_next : symbol_table_lookup(t, "doNext");
		if(s && a->mode & MODE_OPTIMIZATION_ON) {
			generate(h, a, CODE_TOR);
			hole1 = here(h, a);
			assemble(h, a, n->o[0], t, e);
			generate(h, a, OP_CALL | s->value);
			generate(h, a, hole1 << 1);
		} else {
			generate(h, a, CODE_TOR);
			hole1 = here(h, a);
			assemble(h, a, n->o[0], t, e);
			generate(h, a, CODE_RAT);
			hole2 = hole(h, a);
			generate_loop_decrement(h, a, t);
			generate(h, a, OP_BRANCH | hole1);
			fix(h, hole2, OP_0BRANCH | here(h, a));
			generate(h, a, CODE_RDROP);
		}
		break;
	}
	case SYM_FOR_AFT_THEN_NEXT:
	/**@todo sort this out */
		generate(h, a, CODE_TOR);
		assemble(h, a, n->o[0], t, e);
		hole1 = hole(h, a);
		generate(h, a, CODE_RAT);
		generate_loop_decrement(h, a, t);
		hole2 = hole(h, a);
		assemble(h, a, n->o[1], t, e);
		fix(h, hole1, OP_BRANCH | (here(h, a)));
		assemble(h, a, n->o[2], t, e);
		generate(h, a, OP_BRANCH | (hole1 + 1));
		fix(h, hole2, OP_0BRANCH | (here(h, a)));
		generate(h, a, CODE_RDROP);
		break;
	case SYM_BEGIN_WHILE_REPEAT:
		hole1 = here(h, a);
		assemble(h, a, n->o[0], t, e);
		hole2 = hole(h, a);
		assemble(h, a, n->o[1], t, e);
		generate(h, a, OP_BRANCH  | hole1);
		fix(h, hole2, OP_0BRANCH | here(h, a));
		break;
	case SYM_IF1:
		hole1 = hole(h, a);
		assemble(h, a, n->o[0], t, e);
		if(n->o[1]) { /* if ... else .. then */
			hole2 = hole(h, a);
			fix(h, hole1, OP_0BRANCH | (hole2 + 1));
			assemble(h, a, n->o[1], t, e);
			fix(h, hole2, OP_BRANCH  | here(h, a));
		} else { /* if ... then */
			fix(h, hole1, OP_0BRANCH | here(h, a));
		}
		break;
	case SYM_CALL_DEFINITION:
	{
		symbol_t *s = symbol_table_lookup(t, n->token->p.id);
		if(!s)
			assembly_error(e, "not a constant or a defined procedure: %s", n->token->p.id);
		if(s->type == SYMBOL_TYPE_CALL) {
			generate(h, a, OP_CALL | s->value);
		} else if(s->type == SYMBOL_TYPE_CONSTANT || s->type == SYMBOL_TYPE_VARIABLE) {
			generate_literal(h, a, s->value);
		} else {
			error("can only call or push literal: %s", s->id);
			ethrow(e);
		}
		break;
	}
	case SYM_DEFINITION:
		if(n->bits && !(a->mode & MODE_COMPILE_WORD_HEADER))
			assembly_error(e, "cannot modify word bits (immediate/hidden/inline) if not in compile mode");
		if(a->mode & MODE_COMPILE_WORD_HEADER && !(n->bits & DEFINE_HIDDEN)) {
			hole1 = hole(h, a);
			n->bits &= (DEFINE_IMMEDIATE | DEFINE_INLINE);
			fix(h, hole1, a->pwd | (n->bits << 13)); /* shift in word bits into PWD field */
			a->pwd = hole1 << 1;
			pack_string(h, a, n->token->p.id, e);
		}
		symbol_table_add(t, SYMBOL_TYPE_CALL, n->token->p.id, here(h, a), e, n->bits & DEFINE_HIDDEN);
		if(a->in_definition)
			assembly_error(e, "nested word definition is not allowed");
		a->in_definition = true;
		assemble(h, a, n->o[0], t, e);
		generate(h, a, CODE_EXIT);
		a->in_definition = false;
		break;
	case SYM_CHAR: /* [char] A  */
		generate(h, a, OP_LITERAL | n->token->p.id[0]);
		break;
	case SYM_SET:
	{
		uint16_t location, value;
		symbol_t *l = NULL;
		location = literal_or_symbol_lookup(n->token, t, e);

		if(n->value->type == LEX_LITERAL) {
			value = n->value->p.number;
		} else {
			l = symbol_table_lookup(t, n->value->p.id);
			if(l) {
				value = l->value;
				if(l->type == SYMBOL_TYPE_CALL) // || l->type == SYMBOL_TYPE_LABEL)
					value <<= 1;
			} else {
				value = symbol_special(h, a, n->value->p.id, e);
			}
		}
		fix(h, location >> 1, value);
		break;
	}
	case SYM_PWD:
		a->pwd = literal_or_symbol_lookup(n->token, t, e);
		break;
	case SYM_PC:
		h->pc = literal_or_symbol_lookup(n->token, t, e);
		update_fence(a, h->pc);
		break;
	case SYM_MODE:
		a->mode = n->token->p.number;
		break;
	case SYM_ALLOCATE:
		h->pc += literal_or_symbol_lookup(n->token, t, e) >> 1;
		update_fence(a, h->pc);
		break;
	case SYM_BUILT_IN:
		if(!(a->mode & MODE_COMPILE_WORD_HEADER))
			break;

		if(a->built_in_words_defined)
			assembly_error(e, "built in words already defined");
		a->built_in_words_defined = true;

		for(unsigned i = 0; built_in_words[i].name; i++) {
			if(!(built_in_words[i].compile))
				continue;

			if(!built_in_words[i].hidden) {
				uint16_t pwd = a->pwd;
				hole1 = hole(h, a);
				if(built_in_words[i].inline_bit)
					pwd |= (DEFINE_INLINE << 13);
				fix(h, hole1, pwd);
				a->pwd = hole1 << 1;
				pack_string(h, a, built_in_words[i].name, e);
			}
			symbol_table_add(t, SYMBOL_TYPE_CALL, built_in_words[i].name, here(h, a), e, built_in_words[i].hidden);
			for(size_t j = 0; j < built_in_words[i].len; j++)
				generate(h, a, built_in_words[i].code[j]);
			generate(h, a, CODE_EXIT);
		}
		break;
	default:
		fatal("Invalid or unknown type: %u", n->type);
	}
}

static bool assembler(h2_t *h, assembler_t *a, node_t *n, symbol_table_t *t, error_t *e)
{
	assert(h && a && n && t && e);
	if(setjmp(e->j))
		return false;
	assemble(h, a, n, t, e);
	return true;
}

static h2_t *code(node_t *n, symbol_table_t *symbols)
{
	error_t e;
	h2_t *h;
	symbol_table_t *t = NULL;
	assembler_t a;
	assert(n);
	memset(&a, 0, sizeof a);

	t = symbols ? symbols : symbol_table_new();
	h = h2_new(START_ADDR);
	a.fence = h->pc;

	e.jmp_buf_valid = 1;
	if(!assembler(h, &a, n, t, &e)) {
		h2_free(h);
		if(!symbols)
			symbol_table_free(t);
		return NULL;
	}

	if(log_level >= LOG_DEBUG)
		symbol_table_print(t, stderr);
	if(!symbols)
		symbol_table_free(t);
	return h;
}

static h2_t *h2_assemble_core(FILE *input, symbol_table_t *symbols)
{
	assert(input);
	h2_t *h = NULL;
	node_t *n = parse(input);
	if(log_level >= LOG_DEBUG)
		node_print(stderr, n, false, 0);
	if(n)
		h = code(n, symbols);
	node_free(n);
	return h;
}

/* ========================== Assembler ==================================== */

/* ========================== Main ========================================= */

int main(int argc, char **argv)
{
	int r = 0;
	h2_t *h = NULL;
	if(argc == 2) {
		FILE *input = fopen_or_die(argv[1], "rb");
		h = h2_assemble_core(input, NULL);
		if(!h)
			return -1;
		fclose(input);
		save(h, FORTH_BLOCK, h->pc);
	} else {
		h = h2_new(START_ADDR);
		load(h, FORTH_BLOCK);
		r = h2_run(h);
		/*if(!r)
			save(h, FORTH_BLOCK);*/
	}
	h2_free(h);
	return r;
}

/* ========================== Main ========================================= */
