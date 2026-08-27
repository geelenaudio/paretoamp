# Historical patches (GPL §5a)

The driver in `../src/tas5805m.c` is a clean-room re-implementation
against the sonocotta upstream at commit
`c5d70d6ad765a58d798e887c62c3e9cdf49982a0`, not a sequential stack of
`.patch` files. Modification notes live in `../README.md`.

The next upstream sync should drop a `git format-patch` here so the
diff against that snapshot stays reviewable.
