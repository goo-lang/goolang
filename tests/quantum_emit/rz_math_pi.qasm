OPENQASM 3.0;
include "stdgates.inc";
qubit[1] q;
bit[1] c;

rz(0.7853981633974483) q[0];
c[0] = measure q[0];
reset q[0];
