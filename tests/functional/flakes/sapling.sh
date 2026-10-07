#!/usr/bin/env bash

source ./common.sh

[[ $(type -p sl) ]] || skipTest "Sapling not installed"

slInit() {
    sl init "$1"
    sl config --cwd "$1" --local ui.username "Foobar <foobar@example.com>"
}

slCommit() {
    sl commit --cwd "$1" -m "$2"
}

flake1Dir=$TEST_ROOT/flake-sl1
mkdir -p "$flake1Dir"
writeSimpleFlake "$flake1Dir"
slInit "$flake1Dir"

nix registry add --registry "$registry" flake1 "sl+file://$flake1Dir"

flake2Dir=$TEST_ROOT/flake-sl2
mkdir -p "$flake2Dir"
writeDependentFlake "$flake2Dir"
slInit "$flake2Dir"

sl add --cwd "$flake1Dir" .
slCommit "$flake1Dir" 'Initial commit'

sl add --cwd "$flake2Dir" flake.nix
slCommit "$flake2Dir" 'Initial commit'

# Untracked files stay out of the store.
echo untracked > "$flake2Dir/untracked"

nix build -o "$TEST_ROOT/result" "sl+file://$flake2Dir"
[[ -e $TEST_ROOT/result/hello ]]

# Writing the lock file dirtied the tree.
(! nix flake metadata --json "sl+file://$flake2Dir" | jq -e -r .revision)
nix flake metadata --json "sl+file://$flake2Dir" | jq -e -r .dirtyRevision | grep -- '-dirty$'
(! nix eval "sl+file://$flake2Dir"#expr --no-allow-dirty)

# The lock file was added, so committing it needs no explicit `sl add`.
slCommit "$flake2Dir" 'Add lock file'

rev=$(sl log --cwd "$flake2Dir" -r . -T '{node}')
[[ $(nix flake metadata --json "sl+file://$flake2Dir" --refresh | jq -e -r .revision) = "$rev" ]]
[[ $(nix flake metadata --json "sl+file://$flake2Dir" | jq -e -r .revCount) = 2 ]]
nix flake metadata --json "sl+file://$flake2Dir" | jq -e '.lastModified > 0'

nix build -o "$TEST_ROOT/result" "sl+file://$flake2Dir" --no-registries --no-allow-dirty

# The active bookmark is locked as `ref`.
sl bookmark --cwd "$flake2Dir" feature
[[ $(nix flake metadata --json "sl+file://$flake2Dir" | jq -r .locked.ref) = feature ]]

# A plain path into a Sapling working copy resolves to `sl+file`.
[[ $(nix flake metadata --json "$flake2Dir" | jq -r .resolvedUrl) = "sl+file://$flake2Dir" ]]
[[ $(nix flake metadata --json "$flake2Dir" | jq -r .revision) = "$rev" ]]
[[ $(nix eval --impure --raw --expr "(builtins.getFlake \"sl+file://$flake2Dir\").rev") = "$rev" ]]
[[ ! -e "$(nix flake metadata --json "$flake2Dir" | jq -r .path)/untracked" ]]

# `--commit-lock-file` adds and commits a fresh lock file through Sapling.
sl rm --cwd "$flake2Dir" flake.lock
slCommit "$flake2Dir" 'Remove lock file'
nix flake lock "$flake2Dir" --commit-lock-file
[[ -z $(sl status --cwd "$flake2Dir" --modified --added --removed) ]]
sl files --cwd "$flake2Dir" flake.lock

# `nix flake init` adds the new files to a Sapling working copy.
templateDir=$TEST_ROOT/sl-template
mkdir -p "$templateDir/trivial"
cat > "$templateDir/flake.nix" <<EOF2
{
  outputs = { self }: {
    templates.default.path = ./trivial;
    templates.default.description = "trivial";
  };
}
EOF2
echo '{ outputs = { self }: { }; }' > "$templateDir/trivial/flake.nix"

initDir=$TEST_ROOT/sl-init
mkdir -p "$initDir"
slInit "$initDir"
(cd "$initDir" && nix flake init -t "path:$templateDir")
[[ $(sl status --cwd "$initDir" --added --no-status) = flake.nix ]]
