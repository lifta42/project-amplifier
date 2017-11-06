// A simple parser and runtime for minimal LISP.
// Created: 12:35, 2017.10.30 by liftA42.
#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>

#define MAX_NAME_LEN 32
#define ENV_INIT_SIZE 8
#define LIST_MAX_SIZE 32
#define MAX_OPEN_FILE 8 // including stdin and stdout

#define DISPLAY_LIST_MAX_ITEM 3

#define PRE_PARSE_ERROR 1
#define PARSE_ERROR 2
#define RUNTIME_ERROR 3

// Part 1: data structure
// notice: store primitive types in place, refer `struct X`  and strings with
// pointer
enum ElementType {
  TYPE_INT,
  TYPE_BOOL,
  TYPE_NAME,
  TYPE_LIST,
  TYPE_BUILTIN,
  TYPE_LAMBDA,
  TYPE_NULL // nil
};

struct ElementList;
struct Env;

struct Lambda {
  char **args;
  int arg_count;
  struct Env *parent;
  struct ElementList *body;
};

struct Builtin;
typedef struct Element *(*BuiltinFunc)(struct Element **, int,
                                       struct Builtin *);

struct Builtin {
  BuiltinFunc func;
  struct Env *parent;
  void *foreign;
};

struct Element {
  enum ElementType type;
  union {
    int int_value;
    bool bool_value;
    char *name_value;
    struct ElementList *list_value;
    struct Builtin *builtin_value;
    struct Lambda *lambda_value;
  } value;
  // line number and col position help a lot... maybe next level
};

struct ElementList {
  struct Element **elements;
  int length;
};

struct Element *create_element_null() {
  struct Element *ele = malloc(sizeof(struct Element));
  ele->type = TYPE_NULL;
  return ele;
}

// notice: this function do NOT create a `ElementList`
struct Element *create_element_list(int length) {
  struct Element *ele = malloc(sizeof(struct Element));
  ele->type = TYPE_LIST;
  ele->value.list_value = malloc(sizeof(struct ElementList));
  ele->value.list_value->length = length;
  if (length > 0) {
    ele->value.list_value->elements = malloc(sizeof(struct Element *) * length);
  }
  return ele;
}

struct Element *create_element_int(int value) {
  struct Element *ele = malloc(sizeof(struct Element));
  ele->type = TYPE_INT;
  ele->value.int_value = value;
  return ele;
}

struct Element *create_element_bool(bool value) {
  struct Element *ele = malloc(sizeof(struct Element));
  ele->type = TYPE_BOOL;
  ele->value.bool_value = value;
  return ele;
}

struct Element *create_element_builtin(BuiltinFunc func, struct Env *parent) {
  struct Element *ele = malloc(sizeof(struct Element));
  ele->type = TYPE_BUILTIN;
  ele->value.builtin_value = malloc(sizeof(struct Builtin));
  ele->value.builtin_value->func = func;
  ele->value.builtin_value->parent = parent;
  ele->value.builtin_value->foreign = NULL;
  return ele;
}

// rarely create lambda and name, so no util for them

// the following two couples are actually workaround for a fact, that there's
// nothing other than the seven basic types in this language
// I cannot make an array of int, a pointer to int, or a function that takes an
// int as an argument, just like what I can do in C
// thus, at the time I must store something other than those basic types, I have
// to use the TYPE_LIST in a way that it will not be expected to be used

// this guy has a weird interface because its usage: for source code and for
// command line arguments
struct Element *create_string_literal(char *source, int *pos, char until,
                                      int escaped) {
  struct Element **char_seq = malloc(sizeof(struct Element *) * LIST_MAX_SIZE);
  int char_count = 0;
  while (source[*pos] != until) {
    if (char_count == LIST_MAX_SIZE - 1) { // next writing will fill this list
      // perfect code shape is more important than tail recursive
      struct Element *rest = create_string_literal(source, pos, until, escaped);
      struct Element *pack = create_element_list(LIST_MAX_SIZE);
      for (int i = 0; i < LIST_MAX_SIZE - 1; i++) {
        pack->value.list_value->elements[i] = char_seq[i];
      }
      pack->value.list_value->elements[LIST_MAX_SIZE - 1] = rest;
      return pack;
    }

    char c = source[*pos];
    if (!escaped && c == '\\') {
      (*pos)++;
      switch (source[*pos]) {
      case 'n':
        c = '\n';
        break;
      case 't':
        c = '\t';
        break;
      case '\'':
        c = '\'';
        break;
      case '\\':
        c = '\\';
        break;
      default:
        fprintf(stderr, "escape sequence \"\\%c\" is not recognizable\n",
                source[*pos]);
        exit(PARSE_ERROR); // awkward... just keep
      }
    }
    char_seq[char_count] = create_element_int((int)c);
    char_count++;
    (*pos)++;
  }
  assert(char_count < LIST_MAX_SIZE); // there must be space for one more child
  struct Element *pack = create_element_list(char_count + 1);
  for (int i = 0; i < char_count; i++) {
    pack->value.list_value->elements[i] = char_seq[i];
  }
  // end of string symbol
  pack->value.list_value->elements[char_count] = create_element_null();
  return pack;
}

char *unpack_string_literal(struct Element *literal, int *str_length) {
  int expect_length = literal->value.list_value->length;
  // no need to + 1, because there is a end-of-string symbol counted
  char *str = malloc(sizeof(char) * expect_length);
  struct Element *pack = literal;
  int ele_pos = 0;
  *str_length = 0;
  // there is a chance to write this in recursive style, but I prefer not to
  // use `strcat`
  while (1) {
    struct Element *ele = pack->value.list_value->elements[ele_pos];
    if (ele->type == TYPE_LIST) {
      expect_length += ele->value.list_value->length - 1;
      str = realloc(str, sizeof(char) * expect_length);
      pack = ele;
      ele_pos = 0;
    } else if (ele->type == TYPE_NULL) {
      str[*str_length] = '\0';
      break;
    } else {
      str[*str_length] = (char)ele->value.int_value;
      (*str_length)++;
      // `nil` is counted in `expect_length`
      assert(*str_length < expect_length);
      ele_pos++;
      assert(ele_pos < MAX_NAME_LEN); // or it should not go into this branch
    }
  }
  return str;
}

struct Element *create_pointer(void *ptr) {
  struct Element *ele = create_element_list(sizeof(void *));
  // https://www.securecoding.cert.org/confluence/display/c/INT36-C.+Converting+a+pointer+to+integer+or+integer+to+pointer
  uintptr_t p = (uintptr_t)ptr;
  for (int i = 0; i < ele->value.list_value->length; i++) {
    ele->value.list_value->elements[i] = create_element_int(p & 0xff);
    p >>= 8;
  }
  assert(p == 0x0);
  return ele;
}

void *unpack_pointer(struct Element *ele) {
  uintptr_t p = 0x0;
  for (int i = ele->value.list_value->length - 1; i >= 0; i--) {
    p |= (unsigned)ele->value.list_value->elements[i]->value.int_value;
    if (i != 0) {
      p <<= 8;
    }
  }
  return (void *)p;
}

struct EnvPair {
  char *name;
  struct Element *value;
};

struct Env {
  struct EnvPair **values;
  int size;
  int length;
  struct Env *parent;
};

struct Env *create_env(struct Env *parent) {
  struct Env *env = malloc(sizeof(struct Env));
  env->parent = parent;
  env->values = malloc(sizeof(struct EnvPair *) * ENV_INIT_SIZE);
  env->size = ENV_INIT_SIZE;
  env->length = 0;
  return env;
}

struct Element *resolve(char *name, struct Env *env) {
  if (env == NULL) {
    fprintf(stderr, "there is no var called %s\n", name);
    exit(RUNTIME_ERROR);
  }
  for (int i = 0; i < env->length; i++) {
    if (strcmp(name, env->values[i]->name) == 0) {
      return env->values[i]->value;
    }
  }
  return resolve(name, env->parent);
}

void register_(struct Env *env, char *name, struct Element *value) {
  for (int i = 0; i < env->length; i++) {
    if (strcmp(name, env->values[i]->name) == 0) {
      env->values[i]->value = value;
      return;
    }
  }
  if (env->length == env->size) {
    env->values =
        realloc(env->values, sizeof(struct EnvPair *) * env->size * 2);
    env->size *= 2;
  }
  env->values[env->length] = malloc(sizeof(struct EnvPair));
  env->values[env->length]->name = name;
  env->values[env->length]->value = value;
  env->length++;
}

// Part 2: interpret loop
// notice: only modify `Env`s, only create others and refer to existing items
struct Element *eval(struct Element *ele, struct Env *env);
struct Element *apply(struct Element *ele, struct Element **args,
                      int arg_count);

// `lambda`, `quote` and `define` accepts un-evaluated elements as arguments, so
// they are not able to be built-ins, only compiler plugins (macros).
struct Element *eval_lambda(struct Element *lambda, struct Env *parent) {
  assert(lambda->value.list_value->length >= 2);
  struct Element *ele = malloc(sizeof(struct Element));
  ele->type = TYPE_LAMBDA;
  ele->value.lambda_value = malloc(sizeof(struct Lambda));
  ele->value.lambda_value->parent = parent;

  struct Element *arg_list = lambda->value.list_value->elements[1];
  assert(arg_list->type == TYPE_LIST);
  int arg_count = arg_list->value.list_value->length;
  ele->value.lambda_value->arg_count = arg_count;
  ele->value.lambda_value->args = malloc(sizeof(char *) * arg_count);
  for (int i = 0; i < arg_count; i++) {
    assert(arg_list->value.list_value->elements[i]->type == TYPE_NAME);
    ele->value.lambda_value->args[i] =
        arg_list->value.list_value->elements[i]->value.name_value;
  }

  ele->value.lambda_value->body = malloc(sizeof(struct ElementList));
  int body_length = lambda->value.list_value->length - 2;
  ele->value.lambda_value->body->length = body_length;
  ele->value.lambda_value->body->elements =
      malloc(sizeof(struct Element *) * body_length);
  for (int i = 0; i < body_length; i++) {
    ele->value.lambda_value->body->elements[i] =
        lambda->value.list_value->elements[i + 2];
  }
  return ele;
}

struct Element *eval_quote_impl(struct Element *, struct Element *);

struct Element *eval_quote(struct Element *quoted, struct Env *parent) {
  assert(quoted->value.list_value->length == 3);
  struct Element *join = eval(quoted->value.list_value->elements[1], parent),
                 *raw = quoted->value.list_value->elements[2];
  assert(join->type == TYPE_LAMBDA || join->type == TYPE_BUILTIN);
  return eval_quote_impl(raw, join);
}

struct Element *eval_quote_impl(struct Element *raw, struct Element *join) {
  if (raw->type != TYPE_LIST) {
    return raw;
  } else {
    int child_count = raw->value.list_value->length;
    // maybe `children` is a better name
    struct Element **child_list =
        malloc(sizeof(struct Element *) * child_count);
    for (int i = 0; i < child_count; i++) {
      child_list[i] = eval_quote_impl(raw->value.list_value->elements[i], join);
    }
    return apply(join, child_list, child_count);
  }
}

struct Element *eval_define(struct Element *def, struct Env *parent) {
  assert(def->value.list_value->length == 3);
  struct Element *name = def->value.list_value->elements[1];
  // dynamic name is not allowed
  assert(name->type == TYPE_NAME);
  register_(parent, name->value.name_value,
            eval(def->value.list_value->elements[2], parent));

  return create_element_null();
}

struct Element *eval(struct Element *ele, struct Env *env) {
  switch (ele->type) {
  case TYPE_INT:
  case TYPE_BOOL:
  case TYPE_BUILTIN:
  case TYPE_LAMBDA:
  case TYPE_NULL:
    return ele;
  case TYPE_NAME:
    return resolve(ele->value.name_value, env);
  case TYPE_LIST: {
    struct Element *callable = ele->value.list_value->elements[0];
    if (callable->type == TYPE_NAME) {
      if (strcmp(callable->value.name_value, "lambda") == 0) {
        return eval_lambda(ele, env);
      } else if (strcmp(callable->value.name_value, "define") == 0) {
        return eval_define(ele, env);
      } else if (strcmp(callable->value.name_value, "quote") == 0) {
        return eval_quote(ele, env);
      } else {
        callable = resolve(callable->value.name_value, env);
      }
    }
    // IIFE style lambda
    if (callable->type == TYPE_LIST) {
      callable = eval(callable, env);
    }
    int arg_count = ele->value.list_value->length - 1;
    // strict order
    struct Element **args = malloc(sizeof(struct Element *) * arg_count);
    for (int i = 0; i < arg_count; i++) {
      args[i] = eval(ele->value.list_value->elements[i + 1], env);
    }
    return apply(callable, args, arg_count);
  }
  }
}

struct Element *apply(struct Element *ele, struct Element **args,
                      int arg_count) {
  if (ele->type != TYPE_LAMBDA && ele->type != TYPE_BUILTIN) {
    fprintf(stderr, "apply an un-callable element\n");
    exit(RUNTIME_ERROR);
  }
  if (ele->type == TYPE_BUILTIN) {
    // built-in does not need its own env, just use its parent's one if exists
    struct Element *result = ele->value.builtin_value->func(
        args, arg_count, ele->value.builtin_value);
    // TYPE_LIST type value is un-manipulated in this language, unlike the
    // origin Scheme and LISP. It is a good question that whether I should allow
    // built-in to return TYPE_LIST value, since it is able to do so. I forbid
    // it for now because that is trivial.
    assert(result->type != TYPE_LIST);
    return result;
  } else {
    assert(ele->value.lambda_value->arg_count == arg_count);
    // lambda has a static var scope, nothing to do with `parent` argument,
    // which comes from calling scope
    struct Env *env = create_env(ele->value.lambda_value->parent);
    for (int i = 0; i < ele->value.lambda_value->arg_count; i++) {
      register_(env, ele->value.lambda_value->args[i], args[i]);
    }
    struct Element *result =
        create_element_null(); // fallback for (lambda (...) )
    for (int i = 0; i < ele->value.lambda_value->body->length; i++) {
      result = eval(ele->value.lambda_value->body->elements[i], env);
    }
    return result;
  }
}

// Part 3: parser
// notice: create everything here
// all parse_* functions have following signature, they look at input start from
// `source[*pos]` and increase `*pos` during parsing, they leave `source[*pos]`
// to be the first char that they cannot deal with by themselves
struct Element *parse(char *source, int *pos);

void parse_ignore_space(char *source, int *pos) {
  while (isspace(source[*pos])) {
    (*pos)++;
  }
}

struct Element *parse_comment(char *source, int *pos) {
  while (source[*pos] != '\n') {
    (*pos)++;
  }
  (*pos)++;
  while (isspace(source[*pos])) {
    (*pos)++;
  }
  if (source[*pos] == ')') {
    return NULL;
  }
  return parse(source, pos);
}

struct Element *parse_number(char *source, int *pos) {
  // only int actually
  int n = 0;
  while (isdigit(source[*pos])) {
    n = n * 10 + (source[*pos] - '0');
    (*pos)++;
  }
  if (!isspace(source[*pos]) && source[*pos] != '(' && source[*pos] != ')') {
    fprintf(stderr, "name is not allowed to start with digit: \"%d%c...\"\n", n,
            source[*pos]);
    exit(PARSE_ERROR);
  }
  return create_element_int(n);
}

struct Element *parse_string(char *source, int *pos) {
  // (car (list-quoter list 'foo')) ==>
  // (car (list-quoter list (102 111 111 nil))) ==>
  // (car (cons 102 (cons 111 (cons 111 nil)))) ==> 102
  // it must appear inside of `(quote some-join <here>)`, or calling un-callable
  // var (`102` above) exception would be raised use single quotation mark so
  // that `"` can appear without backslash notice: only take effect when
  // following a space, so `fac'` and `six-o'clock` will be normal names
  (*pos)++; // starting quotes
  struct Element *ele = create_string_literal(source, pos, '\'', 0);
  (*pos)++; // ending quotes
  return ele;
}

struct Element *parse_name(char *source, int *pos) {
  // nil is also here

  // now you know why there's limitation about the length of names and lists
  // I do not do any look-ahead in this parser
  char *name = malloc(sizeof(char) * MAX_NAME_LEN);
  int name_len = 0;
  while (!isspace(source[*pos]) && source[*pos] != '(' && source[*pos] != ')') {
    if (name_len == MAX_NAME_LEN - 1) {
      name[name_len] = '\0';
      fprintf(stderr, "name \"%s...\" exceed name length limitation\n", name);
      exit(PARSE_ERROR);
    }
    name[name_len] = source[*pos];
    name_len++;
    (*pos)++;
  }
  name[name_len] = '\0';
  // alternative way is to define a built-in, but I do not like `(nil)`,
  // or to use something like `(define nil ((lambda () (define foo))))`
  // which I do not like either
  if (strcmp(name, "nil") == 0) {
    return create_element_null();
  } else {
    struct Element *ele = malloc(sizeof(struct Element));
    ele->type = TYPE_NAME;
    ele->value.name_value = name;
    return ele;
  }
}

struct Element *parse_list(char *source, int *pos) {
  (*pos)++;
  int child_count = 0;
  struct Element *child_list[LIST_MAX_SIZE];

  parse_ignore_space(source, pos);
  while (source[*pos] != ')') {
    child_list[child_count] = parse(source, pos);
    // check postfix comment
    if (child_list[child_count] != NULL) {
      child_count++;
    }
    parse_ignore_space(source, pos);
  }
  (*pos)++; // for ')'
  struct Element *ele = create_element_list(child_count);
  for (int i = 0; i < child_count; i++) {
    ele->value.list_value->elements[i] = child_list[i];
  }
  return ele;
}

// this function will return NULL (not element with type TYPE_NULL) only in two
// cases:
// 1. reaching '\0' before anything else
// 2. reaching ')' before anything else after parsing a comment line
struct Element *parse(char *source, int *pos) {
  parse_ignore_space(source, pos);

  if (source[*pos] == '\0') {
    return NULL;
  }

  if (source[*pos] == ';') {
    return parse_comment(source, pos);
  }

  if (source[*pos] == ')') {
    fprintf(stderr, "unexpected \")\" appeared\n");
    exit(PARSE_ERROR);
  }

  if (source[*pos] != '(') {
    if (isdigit(source[*pos])) {
      return parse_number(source, pos);
    } else if (source[*pos] == '\'') {
      return parse_string(source, pos);
    } else {
      return parse_name(source, pos);
    }
  } else {
    return parse_list(source, pos);
  }
}

// Part 4: built-in
struct Element *builtin_add(struct Element **args, int arg_count,
                            struct Builtin *self) {
  assert(arg_count == 2);
  assert(args[0]->type == TYPE_INT);
  assert(args[1]->type == TYPE_INT);
  return create_element_int(args[0]->value.int_value +
                            args[1]->value.int_value);
}

struct Element *builtin_debug(struct Element **args, int arg_count,
                              struct Builtin *self) {
  assert(arg_count == 1);
  switch (args[0]->type) {
  case TYPE_INT:
    printf("%d", args[0]->value.int_value);
    break;
  case TYPE_BOOL:
    printf(args[0]->value.bool_value ? "true" : "false");
    break;
  case TYPE_NULL:
    printf("nil");
    break;
  case TYPE_NAME:
    printf("<name: %s>", args[0]->value.name_value);
    break;
  case TYPE_BUILTIN:
    printf("<built-in %p>", args[0]->value.builtin_value);
    break;
  case TYPE_LAMBDA:
    printf("<lambda (");
    char *prefix = "";
    for (int i = 0; i < args[0]->value.lambda_value->arg_count; i++) {
      printf("%s%s", prefix, args[0]->value.lambda_value->args[i]);
      prefix = " ";
    }
    printf(")>");
    break;
  case TYPE_LIST: {
    // it is impossible that a internal list becomes evaled argument
    // however, this is a util for debugging, so anything is possible
    printf("(");
    int item_count = args[0]->value.list_value->length;
    if (item_count > DISPLAY_LIST_MAX_ITEM) {
      item_count = DISPLAY_LIST_MAX_ITEM;
    }
    char *prefix = "";
    for (int i = 0; i < item_count; i++) {
      printf("%s", prefix);
      if (args[0]->value.list_value->elements[i]->type != TYPE_LIST) {
        builtin_debug(&args[0]->value.list_value->elements[i], 1, self);
      } else {
        printf("(...)");
      }
      prefix = " ";
    }
    if (args[0]->value.list_value->length > item_count) {
      printf("%s...", prefix);
    }
    printf(")");
    break;
  }
  }
  return create_element_null();
}

struct Element *builtin_eq(struct Element **args, int arg_count,
                           struct Builtin *self) {
  assert(arg_count == 2);
  assert(args[0]->type == TYPE_INT);
  assert(args[1]->type == TYPE_INT);
  return create_element_bool(args[0]->value.int_value ==
                             args[1]->value.int_value);
}

struct Element *builtin_gt(struct Element **args, int arg_count,
                           struct Builtin *self) {
  assert(arg_count == 2);
  assert(args[0]->type == TYPE_INT);
  assert(args[1]->type == TYPE_INT);
  return create_element_bool(args[0]->value.int_value >
                             args[1]->value.int_value);
}

struct Element *builtin_mul(struct Element **args, int arg_count,
                            struct Builtin *self) {
  assert(arg_count == 2);
  assert(args[0]->type == TYPE_INT);
  assert(args[1]->type == TYPE_INT);
  return create_element_int(args[0]->value.int_value *
                            args[1]->value.int_value);
}

struct Element *builtin_sub(struct Element **args, int arg_count,
                            struct Builtin *self) {
  assert(arg_count == 2);
  assert(args[0]->type == TYPE_INT);
  assert(args[1]->type == TYPE_INT);
  return create_element_int(args[0]->value.int_value -
                            args[1]->value.int_value);
}

struct Element *builtin_is_nil(struct Element **args, int arg_count,
                               struct Builtin *self) {
  assert(arg_count == 1);
  return create_element_bool(args[0]->type == TYPE_NULL);
}

noreturn struct Element *builtin_exit(struct Element **args, int arg_count,
                                      struct Builtin *self) {
  assert(arg_count == 1);
  assert(args[0]->type == TYPE_INT);
  exit(args[0]->value.int_value);
}

#define FOLDR_CALLBACK_REGISTERED_NAME "reserved_foldr-callback"
#define FOLDR_INIT_REGISTERED_NAME "reserved_foldr-init"
struct Element *builtin_foldr_impl(struct Element **, int, struct Builtin *);
// (foldr callback init) ==>
// <env <built-in (args...) ...> >
struct Element *builtin_foldr(struct Element **args, int arg_count,
                              struct Builtin *self) {
  assert(arg_count == 2);
  struct Element *callback = args[0], *init = args[1];
  assert(callback->type == TYPE_LAMBDA || callback->type == TYPE_BUILTIN);

  struct Env *env = create_env(self->parent);
  register_(env, FOLDR_CALLBACK_REGISTERED_NAME, callback);
  register_(env, FOLDR_INIT_REGISTERED_NAME, init);

  return create_element_builtin(builtin_foldr_impl, env);
}

struct Element *builtin_foldr_impl(struct Element **args, int arg_count,
                                   struct Builtin *self) {
  struct Element *callback =
                     resolve(FOLDR_CALLBACK_REGISTERED_NAME, self->parent),
                 *init = resolve(FOLDR_INIT_REGISTERED_NAME, self->parent);
  struct Element *product = init;
  for (int i = arg_count - 1; i >= 0; i--) {
    struct Element *apply_args[2] = {product, args[i]};
    product = apply(callback, apply_args, 2);
  }
  return product;
}

#define PIPE_FUNC_LIST_REGISTERED_NAME "reserved_pipe-func-list"
struct Element *builtin_pipe_impl(struct Element **, int, struct Builtin *);
// (pipe f g) with f :: (lambda (<args: length=x>) ...), g :: (lambda (x) ...)
// ==> (lambda (<args: length=x>) (g (f args...)))
struct Element *builtin_pipe(struct Element **args, int arg_count,
                             struct Builtin *self) {
  assert(arg_count >= 2);
  // `func_list` happens to be a list of elements, it cannot be applied
  struct Element *func_list = create_element_list(arg_count);
  for (int i = 0; i < arg_count; i++) {
    assert(args[i]->type == TYPE_LAMBDA || args[i]->type == TYPE_BUILTIN);
    func_list->value.list_value->elements[i] = args[i];
  }

  struct Env *env = create_env(self->parent);
  register_(env, PIPE_FUNC_LIST_REGISTERED_NAME, func_list);

  return create_element_builtin(builtin_pipe_impl, env);
}

struct Element *builtin_pipe_impl(struct Element **args, int arg_count,
                                  struct Builtin *self) {
  struct Element *func_list =
      resolve(PIPE_FUNC_LIST_REGISTERED_NAME, self->parent);
  struct Element **current_args = args;
  struct Element *current_result = NULL; // to prevent rvalue ref
  int current_arg_count = arg_count;
  for (int i = 0; i < func_list->value.list_value->length; i++) {
    struct Element *func = func_list->value.list_value->elements[i];
    current_result = apply(func, current_args, current_arg_count);
    current_args = &current_result;
    current_arg_count = 1;
  }
  return current_args[0];
}

#define ARGV_REGISTERED_NAME "reserved_argv"
// `cont` postfix stands for continuation-passing-style
// struct Element *builtin_argv_cont(struct Element **args, int arg_count,
//                                   struct Env *parent) {
//   assert(arg_count == 2);
//   struct Element *join_outer = args[0], *join_inner = args[1];
//   assert(join_outer->type == TYPE_BUILTIN || join_outer->type ==
//   TYPE_LAMBDA); assert(join_inner->type == TYPE_BUILTIN || join_inner->type
//   == TYPE_LAMBDA); struct Element *argv = resolve(ARGV_REGISTERED_NAME,
//   parent); int argv_count = argv->value.list_value->length; struct Element
//   **argv_list = malloc(sizeof(struct Element) * argv_count); for (int i = 0;
//   i < argv_count; i++) {
//     // maybe a bad code reuse example
//     argv_list[i] =
//         eval_quote_impl(argv->value.list_value->elements[i], join_inner);
//   }
//   // maybe a bad code not-reuse example
//   return apply(join_outer, argv_list, arg_count);
// }

struct Element *builtin_same(struct Element **args, int arg_count,
                             struct Builtin *self) {
  assert(arg_count == 2);
  assert(args[0]->type == TYPE_NAME && args[1]->type == TYPE_NAME);
  return create_element_bool(
      strcmp(args[0]->value.name_value, args[1]->value.name_value) == 0);
}

struct Element *builtin_if(struct Element **args, int arg_count,
                           struct Builtin *self) {
  assert(arg_count == 3);
  struct Element *cond = args[0], *then = args[1], *other = args[2];
  assert(cond->type = TYPE_BOOL);
  assert(then->type == TYPE_LAMBDA || then->type == TYPE_BUILTIN);
  assert(other->type == TYPE_LAMBDA || other->type == TYPE_BUILTIN);
  return apply(cond->value.bool_value ? then : other, NULL, 0);
}

// struct Element *builtin_write(struct Element **args, int arg_count,
//                               struct Builtin *self) {
//   assert(arg_count == 2);
//   struct Element *file_no = args[0], *output = args[1];
//   assert(file_no->type == TYPE_INT);
//   assert(output->type == TYPE_INT);
//
//   int file_desc = file_no->value.int_value;
//   struct Element *file_table = resolve("_file_table", parent);
//   struct Element *file_info =
//   file_table->value.list_value->elements[file_desc]; if (file_info->type ==
//   TYPE_NULL) {
//     fprintf(stderr, "file descriptor %d does not related to a valid file\n",
//             file_desc);
//     exit(RUNTIME_ERROR);
//   }
//   struct Element *permission = file_info->value.list_value->elements[0];
//   struct Element *file_ptr = file_info->value.list_value->elements[1];
//   if (strstr(permission->value.name_value, "w") == NULL) {
//     fprintf(stderr, "do not have permission to write file %d\n", file_desc);
//     exit(RUNTIME_ERROR);
//   }
//   FILE *file = unpack_pointer(file_ptr);
//   int code = output->value.int_value;
//   assert(isprint(code) || isspace(code));
//   fputc((char)code, file);
//   return create_element_null();
// }

void register_all_orphan_builtin(struct Env *env);
void register_all_related_builtin(struct Env *env, struct Env *with_env);
void register_argv(struct Env *env, int argc, char *argv[]);
char *read_file(char *name, int *len);
void parse_eval(char *source, struct Env *env, int len);
// this is a built-in that actually works as a small driver, so it shared lots
// of driver components
// struct Element *builtin_with(struct Element **args, int arg_count,
//                              struct Env *parent) {
//   //
// }

// struct Element *builtin_main_cont(struct Element **args, int arg_count,
//                                   struct Builtin *self) {
//   assert(arg_count == 1);
//   assert(args[0]->type == TYPE_LAMBDA || args[0]->type == TYPE_BUILTIN);
//   if (args[0]->type == TYPE_LAMBDA) {
//     assert(args[0]->value.lambda_value->arg_count == 1);
//   }
//   struct Element *argv_cont = malloc(sizeof(struct Element));
//   argv_cont->type = TYPE_BUILTIN;
//   argv_cont->value.builtin_value = malloc(sizeof(struct Builtin));
//   argv_cont->value.builtin_value->parent =
//       parent; // this `parent` has raw argv in it
//   argv_cont->value.builtin_value->func = builtin_argv_cont;
//   return apply(args[0], &argv_cont, 1);
// }

struct Element *builtin_trivial_main_cont(struct Element **args, int arg_count,
                                          struct Builtin *self) {
  return create_element_null();
}

struct Element *builtin_string_impl_cont(struct Element **args, int arg_count,
                                         struct Builtin *self) {
  assert(arg_count == 1);
  assert(args[0]->type == TYPE_BUILTIN || args[0]->type == TYPE_LAMBDA);
  return create_element_null();
}

// Part 5: driver
void register_builtin_with_env(struct Env *env, char *name, BuiltinFunc builtin,
                               struct Env *with_env) {
  register_(env, name, create_element_builtin(builtin, with_env));
}

void register_builtin(struct Env *env, char *name, BuiltinFunc builtin) {
  register_builtin_with_env(env, name, builtin, NULL);
}

void register_all_orphan_builtin(struct Env *env) {
  register_builtin(env, "+", builtin_add);
  register_builtin(env, "-", builtin_sub);
  register_builtin(env, "*", builtin_mul);
  register_builtin(env, "debug", builtin_debug);
  register_builtin(env, "=", builtin_eq);
  register_builtin(env, ">", builtin_gt);
  register_builtin(env, "nil?", builtin_is_nil);
  register_builtin(env, "exit", builtin_exit);
  register_builtin(env, "foldr", builtin_foldr);
  register_builtin(env, "pipe", builtin_pipe);
  register_builtin(env, "same", builtin_same);
  register_builtin(env, "if", builtin_if);
}

void register_argv(struct Env *env, int argc, char *argv[]) {
  struct Element *argv_list = create_element_list(argc);
  for (int i = 0; i < argc; i++) {
    int trivial_pos = 0;
    argv_list->value.list_value->elements[i] =
        create_string_literal(argv[i], &trivial_pos, '\0', 1);
  }
  register_(env, ARGV_REGISTERED_NAME, argv_list);
}

char *read_file(char *name, int *len) {
  char *source = NULL;
  *len = 0;
  // read whole file into `source`
  FILE *file = fopen(name, "r");
  if (file == NULL) {
    fprintf(stderr, "can not open file \"%s\"\n", name);
    exit(PRE_PARSE_ERROR);
  }
  while (1) {
    char *line = NULL;
    int line_len = 0;
    line_len = (int)getline(&line, (size_t *)&line_len, file);
    if (line_len < 0) {
      break;
    }
    if (source == NULL) {
      source = line;
    } else {
      source = realloc(source, sizeof(char) * (*len + line_len + 1));
      strcat(source, line);
    }
    *len += line_len;
  }
  fclose(file);
  return source;
}

void parse_eval(char *source, struct Env *env, int len) {
  int pos = 0;
  while (pos != len) {
    struct Element *root = parse(source, &pos);
    if (root != NULL) {
      eval(root, env);
    }
  }
}

struct Env *init_file_env() {
  struct Element *file_stdin = create_element_list(2);
  file_stdin->value.list_value->elements[0] = malloc(sizeof(struct Element));
  file_stdin->value.list_value->elements[0]->type = TYPE_NAME;
  file_stdin->value.list_value->elements[0]->value.name_value = "r";
  file_stdin->value.list_value->elements[1] = create_pointer(stdin);
  struct Element *file_stdout = create_element_list(2);
  file_stdout->value.list_value->elements[0] = malloc(sizeof(struct Element));
  file_stdout->value.list_value->elements[0]->type = TYPE_NAME;
  file_stdout->value.list_value->elements[0]->value.name_value = "w";
  file_stdout->value.list_value->elements[1] = create_pointer(stdout);

  struct Element *file_table = create_element_list(MAX_OPEN_FILE);
  file_table->value.list_value->elements[0] = file_stdin;
  file_table->value.list_value->elements[1] = file_stdout;
  for (int i = 2; i < MAX_OPEN_FILE; i++) {
    file_table->value.list_value->elements[i] = create_element_null();
  }
  struct Env *file_env = create_env(NULL);
  register_(file_env, "_file_table", file_table);
  return file_env;
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Please specify entry source file as command line arguement.\n");
    return 0;
  }

  struct Env *env = create_env(NULL);
  register_all_orphan_builtin(env);

  // reject the solution that register raw argv directly into global env
  // the element that `register_argv` registers to its first argument will have
  // type `TYPE_LIST`, which cannot be manipulated in any way
  // it is considered really bad by me that there is a boulder in my language's
  // global scope, so I create a special env beyond the tree and for argv only
  struct Env *argv_env = create_env(NULL);
  register_argv(argv_env, argc, argv);
  // register_builtin_with_env(env, "main&", builtin_main_cont, argv_env);

  struct Env *file_env = init_file_env();
  // register_builtin_with_env(env, "write", builtin_write, file_env);

  int len = 0;
  char *source = read_file(argv[1], &len);

  parse_eval(source, env, len);
}
