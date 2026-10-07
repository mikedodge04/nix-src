#!/usr/bin/env bash

source common.sh

[[ $(type -p sl) ]] || skipTest "Sapling not installed"

TODO_NixOS

repo=$TEST_ROOT/sl

sl init "$repo"
sl config --cwd "$repo" --local ui.username "Foobar <foobar@example.org>"

fetchSl() {
    nix eval --impure --raw --expr "toString (builtins.fetchTree { type = \"sl\"; url = \"file://$repo\"; $1 }).$2"
}

echo utrecht > "$repo"/hello
sl add --cwd "$repo" hello
sl commit --cwd "$repo" -m 'Bla1'
rev1=$(sl log --cwd "$repo" -r . -T '{node}')

echo world > "$repo"/hello
sl commit --cwd "$repo" -m 'Bla2'
rev2=$(sl log --cwd "$repo" -r . -T '{node}')

# A clean working copy is locked to its parent commit.
path=$(fetchSl "" outPath)
[[ $(cat "$path"/hello) = world ]]
[[ $(fetchSl "" rev) = "$rev2" ]]
[[ $(fetchSl "" revCount) = 2 ]]
[[ $(fetchSl "" lastModified) -gt 0 ]]

# Untracked files are not visible.
echo secret > "$repo"/untracked
expectStderr 1 nix eval --impure --raw --expr "builtins.readFile ((builtins.fetchTree { type = \"sl\"; url = \"file://$repo\"; }) + \"/untracked\")" \
    | grepQuiet "is not tracked by Sapling"
rm "$repo"/untracked

# A dirty working copy has `dirtyRev` and no `rev`.
echo unclean > "$repo"/hello
path=$(fetchSl "" outPath)
[[ $(cat "$path"/hello) = unclean ]]
[[ $(fetchSl "" dirtyRev) = "$rev2-dirty" ]]
[[ $(fetchSl "" dirtyShortRev) = "${rev2:0:7}-dirty" ]]
(! fetchSl "" rev)
sl revert --cwd "$repo" --all

# Fetch an explicit revision, also in pure evaluation mode.
path1=$(fetchSl "rev = \"$rev1\";" outPath)
[[ $(cat "$path1"/hello) = utrecht ]]
[[ $(fetchSl "rev = \"$rev1\";" revCount) = 1 ]]
[[ $(nix eval --raw --expr "builtins.readFile (builtins.fetchTree { type = \"sl\"; url = \"file://$repo\"; rev = \"$rev1\"; } + \"/hello\")") = utrecht ]]

# A bookmark resolves to its commit.
sl bookmark --cwd "$repo" -r "$rev1" feature
[[ $(fetchSl "ref = \"feature\";" rev) = "$rev1" ]]

# Sapling's Git-compatible storage yields the same store path as the `git` fetcher.
gitPath=$(nix eval --impure --raw --expr "(builtins.fetchTree { type = \"git\"; url = \"file://$repo/.sl/store/git\"; rev = \"$rev1\"; }).outPath")
[[ $path1 = "$gitPath" ]]

# Flake-style URLs parse to the same input.
[[ $(nix eval --impure --raw --expr "(builtins.fetchTree \"sl+file://$repo?rev=$rev1\").outPath") = "$path1" ]]
