#pragma once

/* Parses and executes a script's source text, printing output to the
 * console + serial as it runs. Parse/runtime errors are reported the
 * same way rather than propagated - there is no caller that needs a
 * status code yet. */
void script_run(const char *src);
