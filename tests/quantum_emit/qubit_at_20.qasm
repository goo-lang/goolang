OPENQASM 3.0;
include "stdgates.inc";
qubit[21] q;
bit[2] c;

x q[20];
c[0] = measure q[20];
c[1] = measure q[20];
reset q[20];
