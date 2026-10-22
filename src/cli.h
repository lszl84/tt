#pragma once

// Machine-readable subcommands (`tt status`, `tt toggle <task>`, ...) used by
// the Omarchy bar widget. Every command prints one JSON object to stdout.
//
// Returns the process exit code, or -1 if argv names no subcommand and the
// caller should start the app instead.
int RunCli(int argc, char** argv);
