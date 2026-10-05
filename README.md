# xdf

`xdf` is a small X11 disk usage graph in the style of `xload`. It plots the
percentage of space used on the filesystem containing a path. The vertical
scale always runs from 0 to 100%; horizontal lines mark 25%, 50%, and 75%.

Build it with `make` (a C compiler and Xlib development files are required),
then run:

```sh
./xdf                       # root filesystem, refreshed every 10 seconds
./xdf -path /home -update 2 # filesystem containing /home
./xdf -path /mnt/backup -geometry 240x100 -nolabel
./xdf -display :0           # local X screen when DISPLAY is SSH-forwarded
```

Options: `-path`, `-update`, `-label`, `-nolabel`, `-display`, `-geometry`,
`-fg`/`-foreground`, `-bg`/`-background`, and `-hl`/`-highlight`.
Run `./xdf -help` for a summary.

The percentage uses the same denominator as `df`'s `Use%`: used blocks divided
by used blocks plus blocks available to an ordinary user. This means reserved
filesystem space counts as used. The graph retains one sample per pixel of
window width and preserves the newest samples when resized. The label rounds
up to a whole percent, as `df` does.
