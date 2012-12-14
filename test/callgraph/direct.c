
extern int foobar;
extern void baz();

void a();
void b();
void c();
void d();
void e();
void f();

void a() {
  if (foobar) b();
  c();
}

void b() {
  if (!foobar) a();
  c();
}

void c() {
  d();
  e();
}

void d() {
  if (foobar) d();
}

void e() {
}

int main() {
  baz();
  a();
  b();
  return 0;
}

