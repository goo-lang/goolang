OPENQASM 3.0;
include "stdgates.inc";
qubit[2] q;
bit[4] c;

x q[1];
h q[0];
cz q[0], q[1];
h q[0];
c[0] = measure q[0];
reset q[0];
h q[0];
cz q[0], q[1];
s q[0];
if (c[0] == true) {
  sdg q[0];
}
h q[0];
c[1] = measure q[0];
reset q[0];
h q[0];
cz q[0], q[1];
tdg q[0];
if (c[0] == true) {
  tdg q[0];
}
if (c[1] == true) {
  sdg q[0];
}
h q[0];
c[2] = measure q[0];
c[3] = measure q[1];
reset q[1];
