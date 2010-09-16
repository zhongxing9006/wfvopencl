#!/bin/bash
./runPacketizerBlackScholesSimple.sh -q -e -t -i 500
./runPacketizerBitonicSort.sh -q -e -x 131072 -t -i 50
./runPacketizerFastWalshTransform.sh -q -e -x 1000000 -t -i 50
./runPacketizerHistogram.sh -q -e -x 8192 -y 8192 -t -i 50
./runPacketizerMandelbrot.sh -q -e -x 8192 -t
./runPacketizerMatrixTranspose.sh -q -e -x 10000 -t -i 20
./runPacketizerReduction.sh -q -e -t -x 100000 -i 50
