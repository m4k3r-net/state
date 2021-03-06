#ifndef scope_h
#define scope_h

struct dict;

/*
 * Scoped variable dictionary.
 * A scoped dictionary allows us to have "nested" dictionaries, so
 * that variable values can be efficiently restored when they go out of
 * scope.
 *
 * A variable looked up in an inner scope will be sought by looking at
 * all the scopes (inside-out) until it is found.
 *
 * Storing a variable only ever writes it to the inner-most scope. In this
 * way it doesn't destroy the aliased value held in an outer scope.
 * When the inner scope is destroyed,  the value in the outer scope becomes
 * visible again.
 */
struct scope {
	struct scope *outer;
	struct dict *dict;
};

/**
 * Allocates a new variable scope.
 * @param outer   the outer scope that scope_get() will search.
 * @param freefn  a function applied to put values
 *                during #scope_free()
 * @return a new scope that must be released with #scope_free().
 */
struct scope *scope_new(struct scope *outer, void (*freefn)(void *));

/**
 * Looks up a variable in the scope.
 * @param scope   the variable scope
 * @param varname the name of the variable
 * @return the value of the variable, which is BORROWED
 */
void *scope_get(const struct scope *scope, const char * /*atom*/ varname);

/**
 * Stores the value in the current (innermost) scope.
 * @param scope   the variable scope
 * @param varname the name of the variable
 * @param value   the value to store. The dictionary will TAKE
 *                ownership of the value, and free it when the
 *		  scope is freed.
 */
void scope_put(struct scope *scope, const char * /*atom*/ varname,
		void *value);

/**
 * Frees an innermost scope.
 * There must be no scopes inside (i.e. referring to) this scope.
 * @return the outer scope, or @c NULL if none.
 */
struct scope *scope_free(struct scope *scope);

#endif /* scope_h */
