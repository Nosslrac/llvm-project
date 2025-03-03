# OpenMp runtime cloning only the necessary parts
To limit the space usage of the llvm-project when working primarily with the openmp runtime, you can follow the below steps
to only clone the necessary folders.

```sh
git clone --filter=blob:none --no-checkout --depth 10 --sparse <llvm-project-url>
cd llvm-project
```
specify depth that you feel is necessary.

Checkout only the folder that are required for the openmp runtime.
```sh
git sparse-checkout add openmp/ runtimes/ cmake/
git checkout
```

To fetch remote branch to start working on use:

```sh
git fetch origin <rbranch>:<lbranch>
git checkout <lbranch>
```
where ```<rbranch>``` is the remote branch and ```<lbranch>``` is what you name the local branch, usually the same as the remote branch



