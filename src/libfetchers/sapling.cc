#include "nix/fetchers/fetchers.hh"
#include "nix/fetchers/cache.hh"
#include "nix/fetchers/fetch-settings.hh"
#include "nix/fetchers/filtering-source-accessor.hh"
#include "nix/fetchers/git-utils.hh"
#include "nix/util/archive.hh"
#include "nix/util/environment-variables.hh"
#include "nix/util/file-system.hh"
#include "nix/util/logging.hh"
#include "nix/util/memo.hh"
#include "nix/util/os-string.hh"
#include "nix/util/processes.hh"
#include "nix/util/serialise.hh"
#include "nix/util/url-parts.hh"
#include "nix/util/util.hh"

using namespace std::string_literals;

namespace nix::fetchers {

/**
 * Split `s` at the first `sep`; the second half is empty if there is none.
 */
static std::pair<std::string_view, std::string_view> splitOnce(std::string_view s, char sep)
{
    auto i = s.find(sep);
    if (i == s.npos)
        return {s, {}};
    return {s.substr(0, i), s.substr(i + 1)};
}

/**
 * `HGPLAIN` keeps `sl` output stable regardless of user aliases,
 * templates or localization.
 */
static RunOptions slOptions(const std::filesystem::path & repoPath, OsStrings args)
{
    auto env = getEnvOs();
    env[OS_STR("HGPLAIN")] = OS_STR("");

    return {
        .program = "sl",
        .lookupPath = true,
        .args = std::move(args),
        .chdir = repoPath,
        .environment = std::move(env),
    };
}

static std::string runSl(const std::filesystem::path & repoPath, OsStrings args)
{
    auto res = runProgram(slOptions(repoPath, std::move(args)));

    if (!statusOk(res.first))
        throw ExecError(res.first, "sl %1%", statusToString(res.first));

    return res.second;
}

/**
 * `sl files` exits with status 1 when nothing matches, which here just
 * means "untracked".
 */
static bool isTracked(const std::filesystem::path & repoPath, const OsString & relPath)
{
    auto res = runProgram(slOptions(repoPath, {OS_STR("files"), OS_STR("--"), relPath}));
    return statusOk(res.first) && !res.second.empty();
}

static const Hash nullRev{HashAlgorithm::SHA1};

/**
 * The state of a Sapling working copy, as reported by `sl`.
 */
struct SaplingWorkdir
{
    /** Tracked files present on disk, i.e. what `nix` may see. */
    std::set<CanonPath> files;

    /** Whether the working copy differs from its parent commit. */
    bool isDirty;

    /** The working copy's parent commit; `nullRev` before the first commit. */
    Hash headRev;

    /** The active bookmark, if any. */
    std::optional<std::string> bookmark;

    static SaplingWorkdir read(const std::filesystem::path & repoPath)
    {
        auto files = tokenizeString<StringSet>(
            runSl(
                repoPath,
                {OS_STR("status"),
                 OS_STR("--clean"),
                 OS_STR("--modified"),
                 OS_STR("--added"),
                 OS_STR("--no-status"),
                 OS_STR("--print0")}),
            "\0"s);

        auto changes = runSl(
            repoPath,
            {OS_STR("status"), OS_STR("--modified"), OS_STR("--added"), OS_STR("--removed"), OS_STR("--deleted")});

        auto head =
            runSl(repoPath, {OS_STR("log"), OS_STR("-r"), OS_STR("."), OS_STR("-T"), OS_STR("{node} {activebookmark}")});
        auto [node, bookmark] = splitOnce(head, ' ');

        SaplingWorkdir wd{
            .isDirty = !changes.empty(),
            .headRev = Hash::parseAny(node, HashAlgorithm::SHA1),
            .bookmark = bookmark.empty() ? std::nullopt : std::optional{std::string(bookmark)},
        };

        for (auto & file : files)
            wd.files.insert(CanonPath(file));

        return wd;
    }
};

struct SaplingInputScheme : InputScheme
{
    std::optional<Input> inputFromURL(const Settings & settings, const ParsedURL & url, bool requireTree) const override
    {
        if (url.scheme != "sl+file")
            return {};

        auto url2(url);
        url2.scheme = "file";
        url2.query.clear();

        Attrs attrs;
        attrs.emplace("type", "sl");

        for (auto & [name, value] : url.query) {
            if (name == "rev" || name == "ref")
                attrs.emplace(name, value);
            else
                url2.query.emplace(name, value);
        }

        attrs.emplace("url", url2.to_string());

        return inputFromAttrs(settings, attrs);
    }

    std::string_view schemeName() const override
    {
        return "sl";
    }

    std::string schemeDescription() const override
    {
        return stripIndentation(R"(
            Fetch a local [Sapling](https://sapling-scm.com/) working copy that uses Git-compatible storage
            (the default for `sl clone` and `sl init`).

            Only files tracked by Sapling are visible. If the working copy has uncommitted changes, the
            result has `dirtyRev` instead of `rev`, as with the `git` input type.
        )");
    }

    const std::map<std::string, AttributeInfo> & allowedAttrs() const override
    {
        static const std::map<std::string, AttributeInfo> attrs = {
            {"url", {}},
            {"ref", {}},
            {"rev", {}},
            {"revCount", {}},
            {"lastModified", {}},
            {"narHash", {}},
            {"name", {}},
            {"dirtyRev", {}},
            {"dirtyShortRev", {}},
        };
        return attrs;
    }

    std::optional<Input> inputFromAttrs(const Settings & settings, const Attrs & attrs) const override
    {
        auto url = parseURL(getStrAttr(attrs, "url"));

        if (url.scheme != "file")
            throw BadURL("Sapling input '%s' must be a local 'file' URL", url);

        Input input{};
        input.attrs = attrs;
        return input;
    }

    ParsedURL toURL(const Input & input, bool abbreviate) const override
    {
        auto url = parseURL(getStrAttr(input.attrs, "url"));
        url.scheme = "sl+file";
        if (auto rev = input.getRev())
            url.query.insert_or_assign("rev", rev->gitRev());
        if (auto ref = input.getRef())
            url.query.insert_or_assign("ref", *ref);
        return url;
    }

    Input applyOverrides(const Input & input, std::optional<std::string> ref, std::optional<Hash> rev) const override
    {
        auto res(input);
        if (rev)
            res.attrs.insert_or_assign("rev", rev->gitRev());
        if (ref)
            res.attrs.insert_or_assign("ref", *ref);
        return res;
    }

    std::filesystem::path getRepoPath(const Input & input) const
    {
        return urlPathToPath(parseURL(getStrAttr(input.attrs, "url")).path);
    }

    /**
     * Sapling's Git-compatible storage keeps a bare Git repository of
     * the commits, which lets committed state reuse the `git` fetcher's
     * machinery and produce identical store paths.
     */
    ref<GitRepo> openGitStore(const std::filesystem::path & repoPath) const
    {
        auto gitStore = repoPath / ".sl" / "store" / "git";
        if (!pathExists(gitStore))
            throw Error(
                "Sapling repository %s does not use Git-compatible storage, which Nix requires", PathFmt(repoPath));
        return GitRepo::openRepo(gitStore, {.bare = true});
    }

    std::optional<std::filesystem::path> getSourcePath(const Input & input) const override
    {
        return getRepoPath(input);
    }

    void putFile(
        const Input & input,
        const CanonPath & path,
        std::string_view contents,
        std::optional<std::string> commitMsg) const override
    {
        auto repoPath = getRepoPath(input);

        writeFile(repoPath / path.rel(), contents);

        auto relPath = string_to_os_string(std::string(path.rel()));

        if (!isTracked(repoPath, relPath))
            runSl(repoPath, {OS_STR("add"), OS_STR("--"), relPath});

        if (commitMsg) {
            auto [tempFd, tempPath] = createTempFile("nix-msg");
            AutoDelete delTemp(tempPath, /*recursive=*/false);
            writeFull(tempFd.get(), *commitMsg);

            // Pause the logger to allow for user input (such as a signing passphrase) in `sl commit`.
            auto suspension = logger->suspend();
            runSl(repoPath, {OS_STR("commit"), OS_STR("--logfile"), tempPath.native(), OS_STR("--"), relPath});
        }
    }

    static LazyAttr lazyRevCount(ref<GitRepo> gitStore, const Hash & rev)
    {
        return make_ref<LazyAttrComputation>(LazyAttrComputation{
            .compute = memo<ResolvedAttr>([gitStore, rev]() -> ResolvedAttr { return gitStore->getRevCount(rev); }),
        });
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessorFromWorkdir(const Settings & settings, const std::filesystem::path & repoPath, Input && input) const
    {
        auto wd = SaplingWorkdir::read(repoPath);

        ref<SourceAccessor> accessor =
            AllowListSourceAccessor::create(
                makeFSSourceAccessor(repoPath),
                wd.files,
                {CanonPath::root},
                [repoPath](const CanonPath & path) -> RestrictedPathError {
                    if (pathExists(repoPath / path.rel()))
                        return RestrictedPathError(
                            "Path '%1%' in the repository %2% is not tracked by Sapling.\n"
                            "\n"
                            "To make it visible to Nix, run:\n"
                            "\n"
                            "sl add --cwd %2% \"%1%\"",
                            path.rel(),
                            PathFmt(repoPath));
                    return RestrictedPathError(
                        "Path '%s' does not exist in Sapling repository %s.", path.rel(), PathFmt(repoPath));
                })
                .cast<SourceAccessor>();

        auto hasCommits = wd.headRev != nullRev;
        auto gitStore = openGitStore(repoPath);

        if (wd.isDirty) {
            if (!settings.allowDirty)
                throw Error("Sapling tree %s has uncommitted changes", PathFmt(repoPath));

            if (settings.warnDirty)
                warn("Sapling tree %s has uncommitted changes", PathFmt(repoPath));

            if (hasCommits) {
                input.attrs.insert_or_assign("dirtyRev", wd.headRev.gitRev() + "-dirty");
                input.attrs.insert_or_assign("dirtyShortRev", wd.headRev.gitShortRev() + "-dirty");
            }
        } else {
            if (wd.bookmark)
                input.attrs.insert_or_assign("ref", *wd.bookmark);

            input.attrs.insert_or_assign("rev", wd.headRev.gitRev());

            if (hasCommits)
                input.attrs.insert_or_assign("revCount", lazyRevCount(gitStore, wd.headRev));
            else
                input.attrs.insert_or_assign("revCount", uint64_t(0));
        }

        input.attrs.insert_or_assign("lastModified", hasCommits ? gitStore->getLastModified(wd.headRev) : 0);

        return {accessor, std::move(input)};
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessorFromCommit(const std::filesystem::path & repoPath, Input && input) const
    {
        auto gitStore = openGitStore(repoPath);

        auto rev = input.getRev();
        if (!rev) {
            auto ref = *input.getRef();
            auto node = runSl(
                repoPath, {OS_STR("log"), OS_STR("-r"), string_to_os_string(ref), OS_STR("-T"), OS_STR("{node}")});
            rev = Hash::parseAny(node, HashAlgorithm::SHA1);
            input.attrs.insert_or_assign("rev", rev->gitRev());
        }

        if (!gitStore->hasObject(*rev))
            throw Error("Sapling repository %s does not contain commit '%s'", PathFmt(repoPath), rev->gitRev());

        input.attrs.insert_or_assign("revCount", lazyRevCount(gitStore, *rev));
        input.attrs.insert_or_assign("lastModified", gitStore->getLastModified(*rev));

        auto accessor = gitStore->getAccessor(*rev, {}, "«" + input.to_string(true) + "»");

        return {accessor, std::move(input)};
    }

    std::pair<ref<SourceAccessor>, Input>
    getAccessor(const Settings & settings, Store & store, const Input & _input) const override
    {
        Input input(_input);

        auto repoPath = getRepoPath(input);

        return input.getRef() || input.getRev() ? getAccessorFromCommit(repoPath, std::move(input))
                                                : getAccessorFromWorkdir(settings, repoPath, std::move(input));
    }

    std::optional<std::string> getFingerprint(Store & store, const Input & input) const override
    {
        if (auto rev = input.getRev())
            return GitAccessorOptions{}.makeFingerprint(*rev);

        /* A dirty working copy is fingerprinted by its parent commit
           plus the contents of every changed file, so evaluation
           caching still works while editing. */
        auto repoPath = getRepoPath(input);
        auto wd = SaplingWorkdir::read(repoPath);

        auto changed = tokenizeString<StringSet>(
            runSl(
                repoPath,
                {OS_STR("status"),
                 OS_STR("--modified"),
                 OS_STR("--added"),
                 OS_STR("--removed"),
                 OS_STR("--deleted"),
                 OS_STR("--print0")}),
            "\0"s);

        HashSink hashSink{HashAlgorithm::SHA512};
        for (auto & entry : changed) {
            /* Each entry is "<status> <path>". */
            auto [status, path] = splitOnce(entry, ' ');
            auto absPath = repoPath / std::string(path);
            writeString(std::string(status) + ":" + std::string(path), hashSink);
            if (pathExists(absPath))
                dumpPath(absPath, hashSink);
        }

        return GitAccessorOptions{}.makeFingerprint(wd.headRev) + ";sl;d="
               + hashSink.finish().hash.to_string(HashFormat::Base16, false);
    }

    bool isLocked(const Settings & settings, const Input & input) const override
    {
        auto rev = input.getRev();
        return rev && rev != nullRev;
    }
};

static auto rSaplingInputScheme = OnStartup([] { registerInputScheme(std::make_unique<SaplingInputScheme>()); });

} // namespace nix::fetchers
