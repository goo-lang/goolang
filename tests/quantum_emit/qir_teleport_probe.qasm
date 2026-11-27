OPENQASM 3.0;
include "stdgates.inc";
qubit[3] q;
bit[3] c;

ry(1.2345) q[0];
rz(0.6789) q[0];
s q[0];
t q[0];
tdg q[0];
sdg q[0];
rz(-0.6789) q[0];
h q[1];
cx q[1], q[2];
cx q[0], q[1];
h q[0];
c[0] = measure q[0];
c[1] = measure q[1];
if (c[1] == true) {
  x q[2];
}
if (c[0] == true) {
  z q[2];
}
ry(-1.2345) q[2];
reset q[0];
reset q[1];
rx(0) q[0];
y q[0];
y q[0];
x q[0];
x q[1];
ccx q[0], q[1], q[2];
cz q[0], q[1];
cz q[0], q[1];
swap q[0], q[1];
c[2] = measure q[2];
reset q[2];
x q[0];
x q[1];
