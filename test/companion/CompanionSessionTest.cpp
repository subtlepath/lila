#include <gtest/gtest.h>

#include "lib/Companion/CompanionSession.h"
TEST(CompanionSession, AuthenticationAndReconnectFenceResponses) {
  companion::Session session;
  EXPECT_FALSE(session.accepts(0));
  EXPECT_EQ(session.token(), 0);
  ASSERT_TRUE(session.connect());
  EXPECT_FALSE(session.connect());
  EXPECT_EQ(session.token(), 0);
  session.authenticate(true);
  const auto previous = session.token();
  EXPECT_TRUE(session.accepts(previous));
  session.disconnect();
  EXPECT_FALSE(session.accepts(previous));
  session.authenticate(true);
  EXPECT_EQ(session.token(), 0);
  ASSERT_TRUE(session.connect());
  session.authenticate(true);
  EXPECT_NE(session.token(), previous);
  EXPECT_FALSE(session.accepts(previous));
  EXPECT_TRUE(session.accepts(session.token()));
  session.authenticate(false);
  EXPECT_EQ(session.token(), 0);
}
