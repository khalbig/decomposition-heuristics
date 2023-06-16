# Exploiting user-supplied Decompositions inside Heuristics

This repository provides supplementary material on the paper *Exploiting user-supplied Decompositions inside Heuristics* by K. Halbig, A. Göß and D. Weninger.

## Installation

To use the presented heuristics first download the [source code of SCIP][2]. We recommend to download the SCIP Optimization suite since you need an LP solver in addition.

Copy the *.c and *.h files of folder `code` to `path-to-scip-folder/scip/src/scip`. Then you have to include the heuristics in SCIP. For that ensure that each heuristic has corresponding lines in the files
- `path-to-scip-folder/scip/src/scip/scipdefplugins.c`
- `path-to-scip-folder/scip/src/scip/scipdefplugins.h`
- `path-to-scip-folder/scip/src/CMakeLists.txt`
- `path-to-scip-folder/scip/Makefile`.

Install SCIP as usual, we recommend to use CMake. For further help read the [installation instructions][3] of SCIP.

## Usage

In order to use one of the heuristics, you need an instance file (e.g. in LP or MPS format) as well as a corresponding decomposition file (DEC format). See also section [Test set](#test-set).

Using SCIP in a Linux-based terminal, you can type for example

```
./scip -c "read instancename.mps read instancename.dec optimize display statistics q"
```
From the displayed statistics it is possible to extract which of the presented heuristics was called and found a solution.

See the [documentation of SCIP][4] for further instructions.

## Test sets

In the paper we use three different test sets of which two are public.

To get the test set `MIPLIB` download the MIPLIB 2017 benchmark set, which can be found [here][5]. Also the corresponding decompositions of type 'miplib2017' can be found there. The other used three decomposition types can be found in folder `testset`.

Test set `CELLPHONE` is provided by our partner SAP in repository [ibp-sop-benchmarks-milp-cellphoneco][6] in folders `public_mps` and `public_dec`. 


[2]: https://www.scipopt.org/index.php#download

[3]: https://www.scipopt.org/doc/html/INSTALL.php

[4]: https://www.scipopt.org/doc/html/index.php

[5]: https://miplib.zib.de/index.html

[6]: https://github.com/SAP-samples/ibp-sop-benchmarks-milp-cellphoneco
