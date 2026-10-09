"""Host coverage for Aurora's bounded dependency and upgrade transaction."""
import hashlib
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INSTALLER = ROOT / "packages/install.sh"


def make_deb(folder, name, version, files, depends=""):
    folder.mkdir(parents=True)
    control = folder / "control"
    data = folder / "data"
    control.mkdir()
    data.mkdir()
    file_names = sorted(files)
    for name_, content in files.items():
        target = data / name_
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    fields = f"Package: {name}\nVersion: {version}\nArchitecture: musl-linux-amd64\n"
    if depends:
        fields += f"Depends: {depends}\n"
    (control / "control").write_text(fields)
    (control / "files").write_text("\n".join(file_names) + "\n")
    (control / "sha256sums").write_text("".join(
        f"{hashlib.sha256(files[path]).hexdigest()}  {path}\n" for path in file_names
    ))
    for source, output in ((control, folder / "control.tar.gz"), (data, folder / "data.tar.gz")):
        with tarfile.open(output, "w:gz") as archive:
            archive.add(source, arcname=".")
    (folder / "debian-binary").write_text("2.0\n")
    deb = folder / f"{name}.deb"
    subprocess.run(["ar", "crD", str(deb), "debian-binary", "control.tar.gz", "data.tar.gz"], cwd=folder, check=True)
    return deb


def run(root, *args):
    return subprocess.run([str(INSTALLER), "--root", str(root), *map(str, args)], text=True, capture_output=True)


with tempfile.TemporaryDirectory(prefix="aurora-package-lifecycle-") as temporary:
    temporary = Path(temporary)
    root = temporary / "root"
    shared = "opt/aurora/share/lifecycle/value"
    dep = make_deb(temporary / "dep", "aurora-dep", "1.0-1", {"opt/aurora/bin/dep": b"dep\n"})
    app = make_deb(temporary / "app", "aurora-app", "1.0-1", {"opt/aurora/bin/app": b"app\n"}, "aurora-dep")
    result = run(root, app, dep)
    assert result.returncode == 0, result.stderr
    assert (root / "opt/aurora/bin/dep").exists()
    assert (root / "opt/aurora/bin/app").exists()
    assert run(root, "--remove", "aurora-dep").returncode != 0

    repo = temporary / "repo"
    (repo / "apt/dists/aurora/main/binary-musl-linux-amd64").mkdir(parents=True)
    (repo / "apt/pool").mkdir(parents=True)
    shutil.copy2(dep, repo / "apt/pool" / dep.name)
    (repo / "apt/dists/aurora/main/binary-musl-linux-amd64/Packages").write_text(
        f"Package: aurora-dep\nVersion: 1.0-1\nArchitecture: musl-linux-amd64\n"
        f"Filename: pool/{dep.name}\n\n"
    )
    repo_root = temporary / "repo-root"
    result = run(repo_root, "--repository", repo, app)
    assert result.returncode == 0, result.stderr
    assert (repo_root / "opt/aurora/bin/dep").exists()

    missing = make_deb(temporary / "missing", "aurora-missing-app", "1.0-1", {"opt/aurora/bin/missing": b"x\n"}, "aurora-nope")
    result = run(temporary / "missing-root", missing)
    assert result.returncode != 0 and "Missing dependency: aurora-nope" in result.stderr

    cycle_a = make_deb(temporary / "cycle-a", "aurora-cycle-a", "1.0-1", {"opt/aurora/bin/a": b"a\n"}, "aurora-cycle-b")
    cycle_b = make_deb(temporary / "cycle-b", "aurora-cycle-b", "1.0-1", {"opt/aurora/bin/b": b"b\n"}, "aurora-cycle-a")
    result = run(temporary / "cycle-root", cycle_a, cycle_b)
    assert result.returncode != 0 and "Dependency cycle" in result.stderr

    v1 = make_deb(temporary / "v1", "aurora-upgrade", "1.0-1", {shared: b"one\n"})
    v2 = make_deb(temporary / "v2", "aurora-upgrade", "2.0-1", {shared: b"two\n"})
    upgrade_root = temporary / "upgrade-root"
    assert run(upgrade_root, v1).returncode == 0
    assert run(upgrade_root, v2).returncode == 0
    assert (upgrade_root / shared).read_bytes() == b"two\n"
    assert run(upgrade_root, v1).returncode != 0
    assert "Refusing downgrade" in run(upgrade_root, v1).stderr

    collision_root = temporary / "collision-root"
    collision_root.mkdir()
    (collision_root / "opt/aurora/bin").mkdir(parents=True)
    (collision_root / "opt/aurora/bin/app").write_bytes(b"unmanaged\n")
    collision = make_deb(temporary / "collision", "aurora-collision", "1.0-1", {"opt/aurora/bin/app": b"package\n"})
    result = run(collision_root, collision)
    assert result.returncode != 0
    assert (collision_root / "opt/aurora/bin/app").read_bytes() == b"unmanaged\n"

print("PASS package lifecycle: ordering, missing dependencies, cycles, upgrade/downgrade, replacement, rollback and dependency-aware removal")
