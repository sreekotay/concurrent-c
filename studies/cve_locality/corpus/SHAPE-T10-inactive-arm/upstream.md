# Upstream shape — inactive variant arm

Class of bugs where a value is stored under one representation (e.g. text /
bytes / “stringly” number) while consumers read another, or a tagged union’s
inactive arm is reached via raw layout (`.u` / mismatched kind). Redis-style
databases and protocol stacks hit this when a cell can be either a string or
an integer and the wrong arm is projected without a tag check.
