OPENQASM 3.0;
include "stdgates.inc";
qubit[1] q;
bit[66] c;

x q[0];
c[64] = measure q[0];
c[65] = measure q[0];
reset q[0];
