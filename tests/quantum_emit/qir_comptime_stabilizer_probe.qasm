OPENQASM 3.0;
include "stdgates.inc";
qubit[5] q;
bit[2] syn;
bit[12] c;

x q[0];
cx q[0], q[1];
cx q[1], q[2];
cx q[0], q[3];
cx q[1], q[3];
syn[0] = measure q[3];
cx q[1], q[4];
cx q[2], q[4];
syn[1] = measure q[4];
if (syn == 1) {
  x q[0];
}
if (syn == 3) {
  x q[1];
}
if (syn == 2) {
  x q[2];
}
reset q[3];
reset q[4];
cx q[1], q[2];
cx q[0], q[1];
c[0] = measure q[0];
reset q[0];
c[1] = measure q[1];
reset q[1];
c[2] = measure q[2];
reset q[2];
x q[0];
cx q[0], q[1];
cx q[1], q[2];
x q[0];
cx q[0], q[3];
cx q[1], q[3];
syn[0] = measure q[3];
cx q[1], q[4];
cx q[2], q[4];
syn[1] = measure q[4];
if (syn == 1) {
  x q[0];
}
if (syn == 3) {
  x q[1];
}
if (syn == 2) {
  x q[2];
}
reset q[3];
reset q[4];
cx q[1], q[2];
cx q[0], q[1];
c[3] = measure q[0];
reset q[0];
c[4] = measure q[1];
reset q[1];
c[5] = measure q[2];
reset q[2];
x q[0];
cx q[0], q[1];
cx q[1], q[2];
x q[1];
cx q[0], q[3];
cx q[1], q[3];
syn[0] = measure q[3];
cx q[1], q[4];
cx q[2], q[4];
syn[1] = measure q[4];
if (syn == 1) {
  x q[0];
}
if (syn == 3) {
  x q[1];
}
if (syn == 2) {
  x q[2];
}
reset q[3];
reset q[4];
cx q[1], q[2];
cx q[0], q[1];
c[6] = measure q[0];
reset q[0];
c[7] = measure q[1];
reset q[1];
c[8] = measure q[2];
reset q[2];
x q[0];
cx q[0], q[1];
cx q[1], q[2];
x q[2];
cx q[0], q[3];
cx q[1], q[3];
syn[0] = measure q[3];
cx q[1], q[4];
cx q[2], q[4];
syn[1] = measure q[4];
if (syn == 1) {
  x q[0];
}
if (syn == 3) {
  x q[1];
}
if (syn == 2) {
  x q[2];
}
reset q[3];
reset q[4];
cx q[1], q[2];
cx q[0], q[1];
c[9] = measure q[0];
reset q[0];
c[10] = measure q[1];
reset q[1];
c[11] = measure q[2];
reset q[2];
