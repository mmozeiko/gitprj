Toy project for using [Windows Projected File System][ProjFS] to show tree for each commit
of all the tags in git repository without need to manually checkout individual commits.

Make sure `Windows Projected File System` is installed in `Turn Windows features on or off` 

Run `build.cmd` to build executable (requirements - `MSVC`, `git` and `cmake`).
First time it will download and build [libgit2][] library.

To use run `gitprj.exe git_repo_folder` or just drag & drop folder on top of .exe.
It will create `tags` folder with all the tags from repository.

[ProjFS]: https://learn.microsoft.com/en-us/windows/win32/projfs/projected-file-system
[libgit2]: https://libgit2.org
