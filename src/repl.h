/* repl.h — interactive mode: an in-memory workspace of named trees, one
 * active at a time, edited incrementally and switched between. */
#ifndef BPP_TREE_REPL_H
#define BPP_TREE_REPL_H

/* Run the interactive loop, reading commands from stdin. `seed_joins`, if not
 * NULL, is join text used to populate the initial tree. Returns an exit code. */
int repl_run(const char *seed_joins);

#endif /* BPP_TREE_REPL_H */
