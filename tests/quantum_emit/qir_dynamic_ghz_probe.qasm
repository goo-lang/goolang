OPENQASM 3.0;
include "stdgates.inc";
qubit[6] q;
bit[6] c;

h q[0];
cx q[0], q[1];
cx q[1], q[2];
h q[3];
cx q[3], q[4];
cx q[4], q[5];
cx q[2], q[3];
c[0] = measure q[3];
if (c[0] == true) {
  x q[4];
  x q[5];
}
cx q[4], q[5];
cx q[2], q[4];
cx q[1], q[2];
cx q[0], q[1];
h q[0];
c[1] = measure q[0];
reset q[0];
c[2] = measure q[1];
reset q[1];
c[3] = measure q[2];
reset q[2];
c[4] = measure q[4];
reset q[4];
c[5] = measure q[5];
reset q[5];
