#include "state.h"

#include <gtest/gtest.h>
#include <functional>

struct Base {
    using Request = parallel::state::Request<Base>;

    virtual ~Base() = default;
    virtual bool foo() const {
        return true;
    }

    virtual Request oops() = 0;
};

struct Child : Base {
    bool foo() const override {
        return false;
    }

    Request oops() override;
};

struct BadChild : Base {
    Request oops() override;
};

Base::Request Child::oops() {
    return std::make_shared<BadChild>();
}
Base::Request BadChild::oops() {
    return std::make_shared<Child>();;
}

TEST(State, basic) {
    parallel::state::State<Base> graal{parallel::state::Init<Child>()};

    EXPECT_FALSE(graal.call<&Base::foo>());
    EXPECT_TRUE(graal.call<&Base::oops>());
    EXPECT_TRUE(graal.call<&Base::foo>());
}