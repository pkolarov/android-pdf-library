#include "mupdf/html.h"

struct lexbuf
{
	fz_context *ctx;
	const char *s;
	const char *file;
	int line;
	int lookahead;
	int c;
	int color;
	int string_len;
	char string[1024];
};

FZ_NORETURN static void fz_css_error(struct lexbuf *buf, const char *msg)
{
	fz_throw(buf->ctx, FZ_ERROR_GENERIC, "css syntax error: %s (%s:%d)", msg, buf->file, buf->line);
}

static fz_css_rule *fz_new_css_rule(fz_context *ctx, fz_css_selector *selector, fz_css_property *declaration)
{
	fz_css_rule *rule = fz_malloc_struct(ctx, fz_css_rule);
	rule->selector = selector;
	rule->declaration = declaration;
	rule->garbage = NULL;
	rule->next = NULL;
	return rule;
}

static fz_css_selector *fz_new_css_selector(fz_context *ctx, const char *name)
{
	fz_css_selector *sel = fz_malloc_struct(ctx, fz_css_selector);
	sel->name = name ? fz_strdup(ctx, name) : NULL;
	sel->combine = 0;
	sel->cond = NULL;
	sel->left = NULL;
	sel->right = NULL;
	sel->next = NULL;
	return sel;
}

static fz_css_condition *fz_new_css_condition(fz_context *ctx, int type, const char *key, const char *val)
{
	fz_css_condition *cond = fz_malloc_struct(ctx, fz_css_condition);
	cond->type = type;
	cond->key = key ? fz_strdup(ctx, key) : NULL;
	cond->val = val ? fz_strdup(ctx, val) : NULL;
	cond->next = NULL;
	return cond;
}

static fz_css_property *fz_new_css_property(fz_context *ctx, const char *name, fz_css_value *value, int spec)
{
	fz_css_property *prop = fz_malloc_struct(ctx, fz_css_property);
	prop->name = fz_strdup(ctx, name);
	prop->value = value;
	prop->spec = spec;
	prop->next = NULL;
	return prop;
}

static fz_css_value *fz_new_css_value(fz_context *ctx, int type, const char *data)
{
	fz_css_value *val = fz_malloc_struct(ctx, fz_css_value);
	val->type = type;
	val->data = fz_strdup(ctx, data);
	val->args = NULL;
	val->next = NULL;
	return val;
}

static void fz_drop_css_value(fz_context *ctx, fz_css_value *val)
{
	while (val)
	{
		fz_css_value *next = val->next;
		fz_drop_css_value(ctx, val->args);
		fz_free(ctx, val->data);
		fz_free(ctx, val);
		val = next;
	}
}

static void fz_drop_css_condition(fz_context *ctx, fz_css_condition *cond)
{
	while (cond)
	{
		fz_css_condition *next = cond->next;
		fz_free(ctx, cond->key);
		fz_free(ctx, cond->val);
		fz_free(ctx, cond);
		cond = next;
	}
}

static void fz_drop_css_selector(fz_context *ctx, fz_css_selector *sel)
{
	while (sel)
	{
		fz_css_selector *next = sel->next;
		fz_free(ctx, sel->name);
		fz_drop_css_condition(ctx, sel->cond);
		fz_drop_css_selector(ctx, sel->left);
		fz_drop_css_selector(ctx, sel->right);
		fz_free(ctx, sel);
		sel = next;
	}
}

static void fz_drop_css_property(fz_context *ctx, fz_css_property *prop)
{
	while (prop)
	{
		fz_css_property *next = prop->next;
		fz_free(ctx, prop->name);
		fz_drop_css_value(ctx, prop->value);
		fz_free(ctx, prop);
		prop = next;
	}
}

void fz_drop_css(fz_context *ctx, fz_css_rule *rule)
{
	while (rule)
	{
		fz_css_rule *next = rule->next;
		fz_drop_css_selector(ctx, rule->selector);
		fz_drop_css_property(ctx, rule->declaration);
		fz_drop_css_property(ctx, rule->garbage);
		fz_free(ctx, rule);
		rule = next;
	}
}

static void css_lex_next(struct lexbuf *buf)
{
	buf->c = *(buf->s++);
	if (buf->c == '\n')
		++buf->line;
}

static void css_lex_init(fz_context *ctx, struct lexbuf *buf, const char *s, const char *file)
{
	buf->ctx = ctx;
	buf->s = s;
	buf->c = 0;
	buf->file = file;
	buf->line = 1;
	css_lex_next(buf);

	buf->color = 0;
	buf->string_len = 0;
}

static int iswhite(int c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int isnmstart(int c)
{
	return c == '\\' || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= 128 && c <= 255);
}

static int isnmchar(int c)
{
	return c == '\\' || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '-' || (c >= 128 && c <= 255);
}

static void css_push_char(struct lexbuf *buf, int c)
{
	if (buf->string_len + 1 >= nelem(buf->string))
		fz_css_error(buf, "token too long");
	buf->string[buf->string_len++] = c;
}

static int css_lex_accept(struct lexbuf *buf, int t)
{
	if (buf->c == t)
	{
		css_lex_next(buf);
		return 1;
	}
	return 0;
}

static void css_lex_expect(struct lexbuf *buf, int t)
{
	if (!css_lex_accept(buf, t))
		fz_css_error(buf, "unexpected character");
}

static int ishex(int c, int *v)
{
	if (c >= '0' && c <= '9')
	{
		*v = c - '0';
		return 1;
	}
	if (c >= 'A' && c <= 'F')
	{
		*v = c - 'A' + 0xA;
		return 1;
	}
	if (c >= 'a' && c <= 'f')
	{
		*v = c - 'a' + 0xA;
		return 1;
	}
	return 0;
}

static int css_lex_accept_hex(struct lexbuf *buf, int *v)
{
	if (ishex(buf->c, v))
	{
		css_lex_next(buf);
		return 1;
	}
	return 0;
}

static int css_lex_number(struct lexbuf *buf)
{
	while (buf->c >= '0' && buf->c <= '9')
	{
		css_push_char(buf, buf->c);
		css_lex_next(buf);
	}

	if (css_lex_accept(buf, '.'))
	{
		css_push_char(buf, '.');
		while (buf->c >= '0' && buf->c <= '9')
		{
			css_push_char(buf, buf->c);
			css_lex_next(buf);
		}
	}

	if (css_lex_accept(buf, '%'))
	{
		css_push_char(buf, '%');
		css_push_char(buf, 0);
		return CSS_PERCENT;
	}

	if (isnmstart(buf->c))
	{
		css_push_char(buf, buf->c);
		css_lex_next(buf);
		while (isnmchar(buf->c))
		{
			css_push_char(buf, buf->c);
			css_lex_next(buf);
		}
		css_push_char(buf, 0);
		return CSS_LENGTH;
	}

	css_push_char(buf, 0);
	return CSS_NUMBER;
}

static int css_lex_keyword(struct lexbuf *buf)
{
	while (isnmchar(buf->c))
	{
		css_push_char(buf, buf->c);
		css_lex_next(buf);
	}
	css_push_char(buf, 0);
	return CSS_KEYWORD;
}

static int css_lex_string(struct lexbuf *buf, int q)
{
	while (buf->c && buf->c != q)
	{
		if (css_lex_accept(buf, '\\'))
		{
			if (css_lex_accept(buf, 'n'))
				css_push_char(buf, '\n');
			else if (css_lex_accept(buf, 'r'))
				css_push_char(buf, '\r');
			else if (css_lex_accept(buf, 'f'))
				css_push_char(buf, '\f');
			else if (css_lex_accept(buf, '\f'))
				/* line continuation */ ;
			else if (css_lex_accept(buf, '\n'))
				/* line continuation */ ;
			else if (css_lex_accept(buf, '\r'))
				css_lex_accept(buf, '\n');
			else
			{
				css_push_char(buf, buf->c);
				css_lex_next(buf);
			}
		}
		else
		{
			css_push_char(buf, buf->c);
			css_lex_next(buf);
		}
	}
	css_lex_expect(buf, q);
	css_push_char(buf, 0);
	return CSS_STRING;
}

static int css_lex(struct lexbuf *buf)
{
	int t;

	// TODO: keyword escape sequences

	buf->string_len = 0;

	while (buf->c)
	{
restart:
		while (iswhite(buf->c))
			css_lex_next(buf);

		if (buf->c == 0)
			break;

		if (css_lex_accept(buf, '/'))
		{
			if (css_lex_accept(buf, '*'))
			{
				while (buf->c)
				{
					if (css_lex_accept(buf, '*'))
					{
						while (buf->c == '*')
							css_lex_next(buf);
						if (css_lex_accept(buf, '/'))
							goto restart;
					}
					css_lex_next(buf);
				}
				fz_css_error(buf, "unterminated comment");
			}
			return '/';
		}

		if (css_lex_accept(buf, '<'))
		{
			if (css_lex_accept(buf, '!'))
			{
				css_lex_expect(buf, '-');
				css_lex_expect(buf, '-');
				continue; /* ignore CDO */
			}
			return '<';
		}

		if (css_lex_accept(buf, '-'))
		{
			if (css_lex_accept(buf, '-'))
			{
				css_lex_expect(buf, '>');
				continue; /* ignore CDC */
			}
			if (buf->c >= '0' && buf->c <= '9')
			{
				css_push_char(buf, '-');
				return css_lex_number(buf);
			}
			if (isnmstart(buf->c))
			{
				css_push_char(buf, '-');
				css_push_char(buf, buf->c);
				css_lex_next(buf);
				return css_lex_keyword(buf);
			}
			return '-';
		}

		if (css_lex_accept(buf, '+'))
		{
			if (buf->c >= '0' && buf->c <= '9')
				return css_lex_number(buf);
			return '+';
		}

		if (css_lex_accept(buf, '.'))
		{
			if (buf->c >= '0' && buf->c <= '9')
			{
				css_push_char(buf, '.');
				return css_lex_number(buf);
			}
			return '.';
		}

		if (css_lex_accept(buf, '#'))
		{
			int a, b, c, d, e, f;
			if (!css_lex_accept_hex(buf, &a)) goto colorerror;
			if (!css_lex_accept_hex(buf, &b)) goto colorerror;
			if (!css_lex_accept_hex(buf, &c)) goto colorerror;
			if (css_lex_accept_hex(buf, &d))
			{
				if (!css_lex_accept_hex(buf, &e)) goto colorerror;
				if (!css_lex_accept_hex(buf, &f)) goto colorerror;
				buf->color = (a << 20) | (b << 16) | (c << 12) | (d << 8) | (e << 4) | f;
			}
			else
			{
				buf->color = (a << 20) | (b << 12) | (c << 4);
			}
			sprintf(buf->string, "%06x", buf->color);
			return CSS_COLOR;
colorerror:
			fz_css_error(buf, "invalid color");
		}

		if (css_lex_accept(buf, '"'))
			return css_lex_string(buf, '"');
		if (css_lex_accept(buf, '\''))
			return css_lex_string(buf, '\'');

		if (buf->c >= '0' && buf->c <= '9')
			return css_lex_number(buf);

		if (css_lex_accept(buf, 'u'))
		{
			if (css_lex_accept(buf, 'r'))
			{
				if (css_lex_accept(buf, 'l'))
				{
					if (css_lex_accept(buf, '('))
					{
						// string or url
						css_lex_expect(buf, ')');
						return CSS_URI;
					}
					css_push_char(buf, 'u');
					css_push_char(buf, 'r');
					css_push_char(buf, 'l');
					return css_lex_keyword(buf);
				}
				css_push_char(buf, 'u');
				css_push_char(buf, 'r');
				return css_lex_keyword(buf);
			}
			css_push_char(buf, 'u');
			return css_lex_keyword(buf);
		}

		if (isnmstart(buf->c))
		{
			css_push_char(buf, buf->c);
			css_lex_next(buf);
			return css_lex_keyword(buf);
		}

		t = buf->c;
		css_lex_next(buf);
		return t;
	}
	return EOF;
}

static void next(struct lexbuf *buf)
{
	buf->lookahead = css_lex(buf);
}

static int accept(struct lexbuf *buf, int t)
{
	if (buf->lookahead == t)
	{
		next(buf);
		return 1;
	}
	return 0;
}

static void expect(struct lexbuf *buf, int t)
{
	if (accept(buf, t))
		return;
	fz_css_error(buf, "unexpected token");
}

static int iscond(int t)
{
	return t == ':' || t == '.' || t == '#' || t == '[';
}

static fz_css_value *parse_value_list(struct lexbuf *buf);

static fz_css_value *parse_value(struct lexbuf *buf)
{
	fz_css_value *v;

	if (buf->lookahead == CSS_KEYWORD)
	{
		v = fz_new_css_value(buf->ctx, CSS_KEYWORD, buf->string);
		next(buf);

		if (accept(buf, '('))
		{
			v->type = '(';
			v->args = parse_value_list(buf);
			expect(buf, ')');
		}

		return v;
	}

	switch (buf->lookahead)
	{
	case CSS_NUMBER:
	case CSS_LENGTH:
	case CSS_PERCENT:
	case CSS_STRING:
	case CSS_COLOR:
	case CSS_URI:
		v = fz_new_css_value(buf->ctx, buf->lookahead, buf->string);
		next(buf);
		return v;
	}

	if (accept(buf, ','))
		return fz_new_css_value(buf->ctx, ',', ",");
	if (accept(buf, '/'))
		return fz_new_css_value(buf->ctx, '/', "/");

	fz_css_error(buf, "expected value");
}

static fz_css_value *parse_value_list(struct lexbuf *buf)
{
	fz_css_value *head, *tail;

	head = tail = NULL;

	while (buf->lookahead != '}' && buf->lookahead != ';' && buf->lookahead != '!' &&
			buf->lookahead != ')' && buf->lookahead != EOF)
	{
		if (!head)
			head = tail = parse_value(buf);
		else
			tail = tail->next = parse_value(buf);
	}

	return head;
}

static fz_css_property *parse_declaration(struct lexbuf *buf)
{
	fz_css_property *p;

	if (buf->lookahead != CSS_KEYWORD)
		fz_css_error(buf, "expected keyword in property");
	p = fz_new_css_property(buf->ctx, buf->string, NULL, 0);
	next(buf);

	expect(buf, ':');

	p->value = parse_value_list(buf);

	/* !important */
	if (accept(buf, '!'))
		expect(buf, CSS_KEYWORD);

	return p;
}

static fz_css_property *parse_declaration_list(struct lexbuf *buf)
{
	fz_css_property *head, *tail;

	if (buf->lookahead == '}' || buf->lookahead == EOF)
		return NULL;

	head = tail = parse_declaration(buf);

	while (accept(buf, ';'))
	{
		if (buf->lookahead != '}' && buf->lookahead != ';' && buf->lookahead != EOF)
		{
			tail = tail->next = parse_declaration(buf);
		}
	}

	return head;
}

static char *parse_attrib_value(struct lexbuf *buf)
{
	char *s;

	if (buf->lookahead == CSS_KEYWORD || buf->lookahead == CSS_STRING)
	{
		s = fz_strdup(buf->ctx, buf->string);
		next(buf);
		return s;
	}

	fz_css_error(buf, "expected attribute value");
}

static fz_css_condition *parse_condition(struct lexbuf *buf)
{
	fz_css_condition *c;

	if (accept(buf, ':'))
	{
		if (buf->lookahead != CSS_KEYWORD)
			fz_css_error(buf, "expected keyword after ':'");
		c = fz_new_css_condition(buf->ctx, ':', "pseudo", buf->string);
		next(buf);
		return c;
	}

	if (accept(buf, '.'))
	{
		if (buf->lookahead != CSS_KEYWORD)
			fz_css_error(buf, "expected keyword after '.'");
		c = fz_new_css_condition(buf->ctx, '.', "class", buf->string);
		next(buf);
		return c;
	}

	if (accept(buf, '#'))
	{
		if (buf->lookahead != CSS_KEYWORD)
			fz_css_error(buf, "expected keyword after '#'");
		c = fz_new_css_condition(buf->ctx, '#', "id", buf->string);
		next(buf);
		return c;
	}

	if (accept(buf, '['))
	{
		if (buf->lookahead != CSS_KEYWORD)
			fz_css_error(buf, "expected keyword after '['");

		c = fz_new_css_condition(buf->ctx, '[', buf->string, NULL);
		next(buf);

		if (accept(buf, '='))
		{
			c->type = '=';
			c->val = parse_attrib_value(buf);
		}
		else if (accept(buf, '|'))
		{
			expect(buf, '=');
			c->type = '|';
			c->val = parse_attrib_value(buf);
		}
		else if (accept(buf, '~'))
		{
			expect(buf, '=');
			c->type = '~';
			c->val = parse_attrib_value(buf);
		}

		expect(buf, ']');

		return c;
	}

	fz_css_error(buf, "expected condition");
}

static fz_css_condition *parse_condition_list(struct lexbuf *buf)
{
	fz_css_condition *head, *tail;

	head = tail = parse_condition(buf);
	while (iscond(buf->lookahead))
	{
		tail = tail->next = parse_condition(buf);
	}
	return head;
}

static fz_css_selector *parse_simple_selector(struct lexbuf *buf)
{
	fz_css_selector *s;

	if (accept(buf, '*'))
	{
		s = fz_new_css_selector(buf->ctx, NULL);
		if (iscond(buf->lookahead))
			s->cond = parse_condition_list(buf);
		return s;
	}
	else if (buf->lookahead == CSS_KEYWORD)
	{
		s = fz_new_css_selector(buf->ctx, buf->string);
		next(buf);
		if (iscond(buf->lookahead))
			s->cond = parse_condition_list(buf);
		return s;
	}
	else if (iscond(buf->lookahead))
	{
		s = fz_new_css_selector(buf->ctx, NULL);
		s->cond = parse_condition_list(buf);
		return s;
	}

	fz_css_error(buf, "expected selector");
}

static fz_css_selector *parse_adjacent_selector(struct lexbuf *buf)
{
	fz_css_selector *s, *a, *b;

	a = parse_simple_selector(buf);
	if (accept(buf, '+'))
	{
		b = parse_adjacent_selector(buf);
		s = fz_new_css_selector(buf->ctx, NULL);
		s->combine = '+';
		s->left = a;
		s->right = b;
		return s;
	}
	return a;
}

static fz_css_selector *parse_child_selector(struct lexbuf *buf)
{
	fz_css_selector *s, *a, *b;

	a = parse_adjacent_selector(buf);
	if (accept(buf, '>'))
	{
		b = parse_child_selector(buf);
		s = fz_new_css_selector(buf->ctx, NULL);
		s->combine = '>';
		s->left = a;
		s->right = b;
		return s;
	}
	return a;
}

static fz_css_selector *parse_descendant_selector(struct lexbuf *buf)
{
	fz_css_selector *s, *a, *b;

	a = parse_child_selector(buf);
	if (buf->lookahead != ',' && buf->lookahead != '{' && buf->lookahead != EOF)
	{
		b = parse_descendant_selector(buf);
		s = fz_new_css_selector(buf->ctx, NULL);
		s->combine = ' ';
		s->left = a;
		s->right = b;
		return s;
	}
	return a;
}

static fz_css_selector *parse_selector_list(struct lexbuf *buf)
{
	fz_css_selector *head, *tail;

	head = tail = parse_descendant_selector(buf);
	while (accept(buf, ','))
	{
		tail = tail->next = parse_descendant_selector(buf);
	}
	return head;
}

static fz_css_rule *parse_rule(struct lexbuf *buf)
{
	fz_css_selector *s;
	fz_css_property *p;

	s = parse_selector_list(buf);
	expect(buf, '{');
	p = parse_declaration_list(buf);
	expect(buf, '}');
	return fz_new_css_rule(buf->ctx, s, p);
}

static void parse_at_rule(struct lexbuf *buf)
{
	expect(buf, CSS_KEYWORD);

	/* skip until '{' or ';' */
	while (buf->lookahead != EOF)
	{
		if (accept(buf, ';'))
			return;
		if (accept(buf, '{'))
		{
			int depth = 1;
			while (buf->lookahead != EOF && depth > 0)
			{
				if (accept(buf, '{'))
					++depth;
				else if (accept(buf, '}'))
					--depth;
				else
					next(buf);
			}
			return;
		}
		next(buf);
	}
}

static fz_css_rule *parse_stylesheet(struct lexbuf *buf, fz_css_rule *chain)
{
	fz_css_rule *rule, **nextp, *tail;

	tail = chain;
	if (tail)
	{
		while (tail->next)
			tail = tail->next;
		nextp = &tail->next;
	}
	else
	{
		nextp = &tail;
	}

	while (buf->lookahead != EOF)
	{
		if (accept(buf, '@'))
		{
			parse_at_rule(buf);
		}
		else
		{
			rule = *nextp = parse_rule(buf);
			nextp = &rule->next;
		}
	}

	return chain ? chain : tail;
}

fz_css_property *fz_parse_css_properties(fz_context *ctx, const char *source)
{
	struct lexbuf buf;
	css_lex_init(ctx, &buf, source, "<inline>");
	next(&buf);
	return parse_declaration_list(&buf);
}

fz_css_rule *fz_parse_css(fz_context *ctx, fz_css_rule *chain, const char *source, const char *file)
{
	struct lexbuf buf;
	css_lex_init(ctx, &buf, source, file);
	next(&buf);
	return parse_stylesheet(&buf, chain);
}
