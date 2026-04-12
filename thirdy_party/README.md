# thirdy_party

`worm-hole` vendors git-pinned dependencies in two layers:

- `thirdy_party/<name>`
  Direct dependencies used by `worm-hole` itself.
- `thirdy_party/dependencies/<name>`
  Transitive dependencies that exist only because a direct dependency needs them.

Examples:

- `stdexec`, `rapidjson`, `catch2`, `benchmark` are direct dependencies.
- `nlohmann_json` should live under `thirdy_party/dependencies/` if it is only pulled in for `minja`.

Use `tools/thirdy_party_git.sh` to add or pin dependencies so `.gitmodules` and `thirdy_party/LOCK.txt` stay aligned.
