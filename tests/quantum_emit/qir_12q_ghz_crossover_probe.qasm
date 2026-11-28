OPENQASM 3.0;
include "stdgates.inc";
qubit[12] q;
bit[12] c;

h q[0];
cx q[0], q[1];
cx q[1], q[2];
h q[3];
cx q[3], q[4];
cx q[4], q[5];
h q[6];
cx q[6], q[7];
cx q[7], q[8];
h q[9];
cx q[9], q[10];
cx q[10], q[11];
cx q[2], q[3];
cx q[5], q[6];
cx q[8], q[9];
c[0] = measure q[3];
c[1] = measure q[6];
c[2] = measure q[9];
if (c[0] == true) {
  x q[4];
  x q[5];
  x q[7];
  x q[8];
  x q[10];
  x q[11];
}
if (c[1] == true) {
  x q[7];
  x q[8];
  x q[10];
  x q[11];
}
if (c[2] == true) {
  x q[10];
  x q[11];
}
cx q[10], q[11];
cx q[8], q[10];
cx q[7], q[8];
cx q[5], q[7];
cx q[4], q[5];
cx q[2], q[4];
cx q[1], q[2];
cx q[0], q[1];
h q[0];
c[3] = measure q[0];
reset q[0];
c[4] = measure q[1];
reset q[1];
c[5] = measure q[2];
reset q[2];
c[6] = measure q[4];
reset q[4];
c[7] = measure q[5];
reset q[5];
c[8] = measure q[7];
reset q[7];
c[9] = measure q[8];
reset q[8];
c[10] = measure q[10];
reset q[10];
c[11] = measure q[11];
reset q[11];
