# Shinto

After any rebuild of `app/build/shinto-bin`, deploy it and bounce the warm
daemon:

```
./shinto restart
```

`shinto.service` runs `~/.local/share/shinto/src`, a separate clone. A rebuild
in this tree is invisible until `restart` copies the new binary over. Existing
Shinto windows close with the old process.

Commit small logical changes as you go, without waiting to be asked. One
concern per commit. Push when the user asks, or when the change is already
something they wanted landed.
