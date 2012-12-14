
extern unsigned foobar;
extern void baz();

void c1(int a, char *b, short c) {
}

void c2(int a, char *b, short c) {
}

void c3(int a, char *b, short c) {
}

void c4(int a, char *b, short c) {
}


void a() {
  void (*fptr)(int, char *, short);
  switch(foobar) {
   case 0:fptr = c1;break;
   case 1:fptr = c2;break;
   default:fptr = c3;break;
  }
  fptr(1, "hello", 2);
}

void d1(int a, char *b, char* c) {
}

void d2(int a, int *b, short c) {
}

void d3(int a, char *b) {
}

void d4(int a, char *b, char *c) {
}


void b() {
  void (*fptr)(int, char *, char *);
  switch(foobar) {
   case 0:fptr = d1;break;
   case 1:fptr = d2;break;
   default:fptr = d3;break;
  }
  fptr(1, "hello", "goodbye");
}

int main() {
  baz();
  a();
  b();
  c4(2, "hello", 3);
  d4(2, "hello", 3);
}

