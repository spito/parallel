% Memory & parallelism
% How to get a core file easily and swiftly
% Author CPBR

# What was then

~~~cpp
void *malloc(size_t size);
void free(void *ptr);
~~~

 * Want to get memory? Just call `malloc()`.
 * Want to return it? Just call `free()`.
 * Easy, isn't it?


# Well...

~~~{.c .numberLines}
struct matrix *create(int rows, int cols) {
  struct matrix *data = malloc(sizeof(*data));
  if (!data)
    return NULL;
  memset(data, 0, sizeof(*data)); data->cols = cols;
  data->data = malloc(sizeof(int) * rows);
  if (!data->data) { free(data); return NULL;}
  for (; data->rows != rows; ++data->rows) {
    data->data[data->rows] = malloc(sizeof(int) * cols);
    if (!data->data[data->rows]) break;
  }
  if (data->rows != rows) { // code duplication
    while (data->rows --> 0) free(data->data[data->rows]);
    free(data->data); free(data);
    return NULL;
  }
  return data;
}
~~~

# We have new & delete & ctors & dtors. Hooray!

~~~{.cpp .numberLines}
struct Foo {
    Foo()
        : a(new int(10))
        , b(new int(20))
    {}
    ~Foo() {
        delete a;
        delete b;
    }
private:
    int *a;
    int *b;
};
~~~

**Q:** Is it correct?

# We have new & delete & ctors & dtors. Hooray!

~~~{.cpp .numberLines}
struct Foo {
    Foo()
        : a(new int(10)) // never deleted
        , b(new int(20)) // if this throws
    {}
    ~Foo() {
        delete a;
        delete b;
    }
private:
    int *a;
    int *b;
};
~~~

**Q:** Is it correct? **A:** No!

# Correct solution

~~~{.cpp .numberLines}
struct Foo {
    Foo()
        : a(std::make_unique<int>(10))
        , b(std::make_unique<int>(20))
    {}
    // implicit dtor
private:
    std::unique_ptr<int> a;
    std::unique_ptr<int> b;
};
~~~

 * `std::unique_ptr<T>`
 * `std::shared_ptr<T>`
 * `std::optional<T>`

# Why this?

 * you should know it
 * it is a generic concept
    * RAII (C++)
    * try-with-resources (Java, Kotlin)
    * with-statement (Python)
    * using (C#)
 * useful not just with memory
    * mutexes
    * sockets
    * files
    * ...

# Parallelism in examples (for newbies)

~~~{.cpp .numberLines}
int i = 0;
std::thread t([&]{ ++i; });
++i;
t.join();

assert(i == 2);
~~~

**Q:** Is it correct?
\onslide<2-> **A:** No

\onslide<2-> Increment is not an atomic operation.

\onslide<2-> In case you would like to count things in a parallel environment, use `std::atomic<T>`.

# Parallelism in examples (MCO shutdown pattern)

~~~{.cpp .numberLines}
void waitAndGo(bool &value) {
    while (!value) sleep(1);
    go();
}

bool done = false;
std::thread t([&]{ doSmth(); done = true; });

waitAndGo(done);

t.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** No

\onslide<2-> Line 2 could be optimized-out.

# Parallelism in examples (advanced)

~~~{.cpp .numberLines}
struct Foo {
    Foo() : data(loadData()) {}
    int countIf(int condition) const;
    bool reload();
private:
    std::map<std::string, std::vector<Bar>> data;
};

Foo foo;
std::thread t1([&] { process(foo.countIf(10)); });
std::thread t2([&] { process(foo.countIf(20)); });

t1.join();
t2.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** Yes

\onslide<2-> Compiler expect const access to be thread safe.

# Parallelism in examples (advanced)

~~~{.cpp .numberLines}
struct Foo {
    Foo() : data(loadData()) {}
    int countIf(int condition) const;
    bool reload();
private:
    std::map<std::string, std::vector<Bar>> data;
};

Foo foo;
std::thread t1([&] { process(foo.countIf(10)); });
std::thread t2([&] { process(foo.countIf(20)); });
foo.reload();
t1.join();
t2.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** No

\onslide<2-> High probability of corruption of `std::map` with core file.

# Parallelism in examples (with memory)

~~~{.cpp .numberLines}
auto foo = std::make_shared<Foo>();

std::thread t1([&]{ foo->doSmth(); });
std::thread t2([&]{ foo->doBar(); });

foo.reset();
t1.join();
t2.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** No

\onslide<2-> `std::shared_ptr` is not generally thread safe!

# Parallelism in examples (with memory)

~~~{.cpp .numberLines}
auto foo = std::make_shared<Foo>();

std::thread t1([=]{ foo->doSmth(); });
std::thread t2([=]{ foo->doBar(); });

foo.reset();
t1.join();
t2.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** Yes

\onslide<2-> Different instances of the same object are handled properly.

# Parallelism in examples (sockets)

~~~{.cpp .numberLines}
struct Session : std::enable_shared_from_this<Session> {
    void onConnect(Conn conn) {
        _connection = conn;
        auto self = shared_from_this();
        _connection->onData([self](std::string_view data){
            self->onData(data);
        });
    }
};
~~~

**Q:** Is it correct?
\onslide<2-> **A:** No

\onslide<2-> From the parallelism perspective, it is correct. However, it leaks every `Session`.

# Parallelism in examples (sockets)

~~~{.cpp .numberLines}
struct Session : std::enable_shared_from_this<Session> {
    void onConnect(Conn conn) {
        _connection = conn;
        std::weak_ptr<Session> ref = shared_from_this();
        _connection->onData([ref](std::string_view data){
            if (auto self = ref.lock())
                self->onData(data);
        });
    }
};
~~~

**Q:** Is it correct?
\onslide<2-> **A:** Yes

# Parallelism in examples (design)

~~~{.cpp .numberLines}
struct Foo {
    std::mutex m;
    int i;
};
Foo f;
std::thread t1([&]{ f.m.lock(); ++f.i; f.m.unlock(); });
std::thread t2([&]{ f.m.lock(); ++f.i; f.m.unlock(); });

t1.join(); t2.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** Yes, but it is ugly.

\onslide<3-> What about RAII? And what about encapsulation?

# Parallelism in examples (design)

~~~{.cpp .numberLines}
struct Foo {
    void doSmth() {
        std::unique_lock<std::mutex> lock(m);
        ++i;
    }
private:
    std::mutex m;
    int i;
};
Foo f;
std::thread t1([&]{ f.doSmth(); });
std::thread t2([&]{ f.doSmth(); });

t1.join(); t2.join();
~~~

**Q:** Is it correct?
\onslide<2-> **A:** Yes

# Parallelism in examples (design)

~~~{.cpp .numberLines}
struct Foo {
    void doSmth1() {
        std::unique_lock<std::mutex> lock(m);
        done = true;
    }
    void doSmth2() {
        std::unique_lock<std::mutex> lock(m);
        if (done) { ++i; done = false; }
    }
private:
    std::mutex m;
    bool done;
    int i;
};
Foo f;
std::thread t1([&]{ f.doSmth1(); f.doSmth2(); });
std::thread t2([&]{ f.doSmth1(); f.doSmth2(); });
t1.join(); t2.join();
~~~

# Parallelism in examples (design) continue...

**Q:** Was it correct?

\pause

**A:** No!

**Q:** Can we do better?

\pause

**A:** Yes, we can! We could wrap attributes of `Foo` so every access to it is guarded by mutex.

~~~{.cpp .numberLines}
#include "parallel_guard.h"

struct Foo {
    struct Self {
        bool done; int i;
    };
    mco::guard::Exclusive<Self> self;
};
~~~

# Parallelism in examples (design) continue...

~~~{.cpp .numberLines}
struct Foo {
    void doSmth() {
        self.access([](auto &self) {
            self.doSmth1();
            self.doSmth2();
        });
    }
private:
    struct Self {
        void doSmth1() { done = true; }
        void doSmth2() {
            if (done) { ++i; done = false;}
        }
    private:
        bool done; int i;
    };
    mco::guard::Exclusive<Self> self;
};
~~~

# Parallel guard

 * `mco::guard::Exclusive`
    * standard (exclusive) mutex semantic
 * `mco::guard::Shared`
    * shared (read-write) mutex semantic
 * `mco::guard::Recursive`
    * recursive mutex semantic

# Parallel guard

## Use accessTo method to obtain proxy

~~~{.cpp .numberLines}
mco::guard::Shared<Object> smth;
smth.accessTo().object().someMethod();

auto proxy = smth.constAccessTo();
proxy.object().constMethod1();
proxy.object().constMethod2();
~~~

## Pass a functor to the access method

~~~{.cpp .numberLines}
mco::guard::Exclusive<Object> smth;
smth.access([&](auto &object) {
    object.someMethod();
});
~~~

# Parallel guard

In case you need to lock more guarded objects at once and need to avoid deadlock, use lockAll function

~~~{.cpp .numberLines}
auto result = mco::guard::lockAll(
    [](auto &object1, auto &object2) {
        return object1.someOperation()
        + object2.someOtherOperation();
}, guardedObject1, guardedObject2);
~~~