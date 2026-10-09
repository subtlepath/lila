#include <Memory.h>
#include <gtest/gtest.h>

#include "ChapterXPathResolver.h"
#include "ProgressContentAnchorResolver.h"
#include "src/CompanionBookmarkLegacyAnchor.h"
#include "src/CompanionBookmarkReaderContext.h"

TEST(ProgressContentAnchor, ResolvesNestedAndRepeatedAnchorsWithoutPageEstimates) {
  Epub epub({"<html><body><div><p>Alpha bravo</p><p>Second</p></div></body></html>"});
  auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
  ASSERT_TRUE(resolver);
  XPathContentAnchor output{9, 99};
  ASSERT_TRUE(resolver->resolve(epub, "/body/DocFragment[1]/body/div[1]/p[1]/text()[1].6", output));
  EXPECT_EQ(output, (XPathContentAnchor{0, 6}));
  ASSERT_TRUE(resolver->resolve(epub, "/body/DocFragment[1]/body/div[1]/p[2]/text()[1].2", output));
  EXPECT_GT(output.visibleTextOffset, 6u);
  ASSERT_TRUE(resolver->resolve(epub, "/body/DocFragment[1]/body", output));
  EXPECT_EQ(output, (XPathContentAnchor{0, 0}));
}
TEST(ProgressContentAnchor, RejectsMalformedMissingAndOversizedPathsWithoutChangingOutput) {
  Epub epub({"<html><body><p>Alpha</p></body></html>"});
  auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
  ASSERT_TRUE(resolver);
  for (const auto path :
       {"", "/body/DocFragment[2]/body", "/body/DocFragment[999999999999999]/body",
        "/body/DocFragment[1]/body/p[1]/text()[1].999999999999999",
        "/body/DocFragment[1]/body/p[999999999999999]/text()[1].0", "/body/DocFragment[1]/body/p[1]/text()[1].bad",
        "/body/DocFragment[1]/body/p[2]/text()[1].0", "/body/DocFragment[1]/body/p[]/text()[1].0",
        "/body/DocFragment[1]/body/p[0]/text()[1].0", "/body/DocFragment[1]/body/p[1]junk/text()[1].0",
        "/body/DocFragment[1]/body/p[1]/text()[bad].0"}) {
    XPathContentAnchor output{9, 99};
    EXPECT_FALSE(resolver->resolve(epub, path, output));
    EXPECT_EQ(output, (XPathContentAnchor{9, 99}));
  }
  std::string deep = "/body/DocFragment[1]/body/";
  for (int i = 0; i < 17; ++i) deep += "div[1]/";
  deep += "text()[1].0";
  XPathContentAnchor output{9, 99};
  EXPECT_FALSE(resolver->resolve(epub, deep, output));
  EXPECT_EQ(output, (XPathContentAnchor{9, 99}));
}

TEST(ProgressContentAnchor, ContainerOffsetIsResolvedInsteadOfAssumedChapterStart) {
  Epub epub({"<html><body><div><p>First</p></div><div><p>Second</p></div></body></html>"});
  auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
  XPathContentAnchor output;
  ASSERT_TRUE(resolver->resolve(epub, "/body/DocFragment[1]/body/div[2].0", output));
  EXPECT_EQ(output.spineIndex, 0);
  EXPECT_GT(output.visibleTextOffset, 0u);
}
TEST(ProgressContentAnchor, NativeAdapterUpdatesOnlyResolvedAnchorFields) {
  Epub epub({"<html><body><p>Alpha</p></body></html>"});
  auto resolver = makeUniqueNoThrow<companion::NativeBookmarkLegacyAnchor>();
  ASSERT_TRUE(resolver);
  BookmarkEntry entry{};
  entry.xpath = "/body/DocFragment[1]/body/p[1]/text()[1].2";
  entry.name = "Name";
  entry.summary = "Summary";
  entry.computedSpineIndex = 9;
  ASSERT_TRUE(resolver->resolve(epub, entry));
  EXPECT_TRUE(entry.hasVisibleTextOffset);
  EXPECT_EQ(entry.computedSpineIndex, 0);
  EXPECT_EQ(entry.visibleTextOffset, 2u);
  EXPECT_EQ(entry.name, "Name");
  EXPECT_EQ(entry.summary, "Summary");
  entry.hasVisibleTextOffset = false;
  entry.xpath = "bad";
  entry.visibleTextOffset = 99;
  EXPECT_FALSE(resolver->resolve(epub, entry));
  EXPECT_FALSE(entry.hasVisibleTextOffset);
  EXPECT_EQ(entry.visibleTextOffset, 99u);
}

TEST(ProgressContentAnchor, RoundtripsVisibleOffsetsAcrossXmlTextBoundaries) {
  for (const auto source : {"<html><body><p>Alpha bravo</p></body></html>",
                            "<html><body><div><p>Second <em>nested</em> tail</p></div></body></html>",
                            "<html><body><p>before<!--comment-->after</p></body></html>",
                            "<html><body><p>before<?marker?>after</p></body></html>",
                            "<html><body><p>before<![CDATA[middle]]>after</p></body></html>",
                            "<html><body><p>before<rp>hidden</rp>after</p></body></html>",
                            "<html><body><p>One &amp; two &#233; three</p></body></html>"}) {
    auto epub = std::make_shared<Epub>(std::vector<std::string>{source});
    auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
    for (uint32_t offset = 0; offset < 10; ++offset) {
      const auto xpath = ChapterXPathResolver::findXPathForVisibleTextOffset(epub, 0, offset);
      ASSERT_FALSE(xpath.empty());
      XPathContentAnchor output{9, 99};
      ASSERT_TRUE(resolver->resolve(*epub, xpath, output)) << xpath << " in " << source;
      EXPECT_EQ(output.visibleTextOffset, offset) << xpath << " in " << source;
    }
  }
}

TEST(ProgressContentAnchor, CountsAllVisibleBodyCodepointsLikeReaderAndRefusesCorruptXml) {
  Epub epub({"<html><body>\n<h1>Title</h1><p>Alpha</p></body></html>"});
  auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
  XPathContentAnchor output{9, 99};
  ASSERT_TRUE(resolver->resolve(epub, "/body/DocFragment[1]/body/p[1]/text()[1].2", output));
  EXPECT_EQ(output.visibleTextOffset, 8u);
  Epub broken({"<html><body><p>Alpha</p><broken></body></html>"});
  output = {9, 99};
  EXPECT_FALSE(resolver->resolve(broken, "/body/DocFragment[1]/body/p[1]/text()[1].2", output));
  EXPECT_EQ(output, (XPathContentAnchor{9, 99}));
  std::string deep = "<html><body>";
  for (int i = 0; i < 65; ++i) deep += "<div>";
  deep += "<p>Alpha</p>";
  for (int i = 0; i < 65; ++i) deep += "</div>";
  deep += "</body></html>";
  Epub nested({deep});
  EXPECT_FALSE(resolver->resolve(nested, "/body/DocFragment[1]/body/p[1]/text()[1].2", output));
  EXPECT_EQ(output, (XPathContentAnchor{9, 99}));
}

TEST(ProgressContentAnchor, MissingExactAncestryDoesNotSelectNestedLookalikes) {
  Epub epub({"<html><body><div><p>Other</p></div><section><div><p>Another</p></div></section></body></html>"});
  auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
  ASSERT_TRUE(resolver);
  for (const auto path :
       {"/body/DocFragment[1]/body/p[1]/text()[1].2", "/body/DocFragment[1]/body/div[2]/p[1]/text()[1].2"}) {
    XPathContentAnchor output{9, 99};
    EXPECT_FALSE(resolver->resolve(epub, path, output));
    EXPECT_EQ(output, (XPathContentAnchor{9, 99}));
  }
  XPathContentAnchor output{9, 99};
  ASSERT_TRUE(resolver->resolve(epub, "/body/DocFragment[1]/body/div[1]/p[1]/text()[1].2", output));
  EXPECT_EQ(output, (XPathContentAnchor{0, 2}));
}

TEST(ProgressContentAnchor, UnindexedPathsRequireOneMatchingElement) {
  auto resolver = makeUniqueNoThrow<ProgressContentAnchorResolver>();
  ASSERT_TRUE(resolver);
  for (const auto source : {"<html><body><div><p>Alpha</p></div><div><p>Bravo</p></div></body></html>",
                            "<html><body><div><p>A</p></div><div><p>Bravo</p></div></body></html>"}) {
    Epub epub({source});
    XPathContentAnchor output{9, 99};
    EXPECT_FALSE(resolver->resolve(epub, "/body/DocFragment[1]/body/div/p[1]/text()[1].2", output));
    EXPECT_EQ(output, (XPathContentAnchor{9, 99}));
  }
  Epub unique({"<html><body><div><p>Alpha</p></div></body></html>"});
  XPathContentAnchor output{9, 99};
  ASSERT_TRUE(resolver->resolve(unique, "/body/DocFragment[1]/body/div/p[1]/text()[1].2", output));
  EXPECT_EQ(output, (XPathContentAnchor{0, 2}));
}

namespace {
class ReaderContextIdentity final : public companion::IdentityStorage {
 public:
  ReaderContextIdentity() {
    identity.device.fill(1);
    identity.storageGeneration.fill(4);
    identity.eventEpoch = 1;
    card.fill(2);
    marker.fill(3);
    record[0] = 'L';
    record[1] = 'C';
    record[2] = 'I';
    record[3] = 1;
    std::copy(identity.device.begin(), identity.device.end(), record.begin() + 4);
    std::copy(card.begin(), card.end(), record.begin() + 20);
    std::copy(marker.begin(), marker.end(), record.begin() + 36);
    std::copy(identity.storageGeneration.begin(), identity.storageGeneration.end(), record.begin() + 52);
    record[68] = 1;
    companion::bookmark_record_detail::put(std::span(record).last(4),
                                           companion::bookmark_record_detail::checksum(std::span(record).first(76)));
  }
  bool hardwareIdentity(companion::Identity& output) override {
    output = identity.device;
    return true;
  }
  bool cardIdentity(companion::Identity& output) override {
    output = card;
    return true;
  }
  companion::IdentityRead readBinding(std::span<uint8_t> output) override {
    std::copy(record.begin(), record.end(), output.begin());
    return companion::IdentityRead::Present;
  }
  companion::IdentityRead readMarker(companion::Identity& output) override {
    output = marker;
    return companion::IdentityRead::Present;
  }
  bool writeBinding(std::span<const uint8_t>) override {
    ++mutations;
    return false;
  }
  bool createMarker(const companion::Identity&) override {
    ++mutations;
    return false;
  }
  bool randomIdentity(companion::Identity&) override {
    ++mutations;
    return false;
  }
  companion::IdentityState identity;
  companion::Identity card{}, marker{};
  std::array<uint8_t, companion::IDENTITY_RECORD_SIZE> record{};
  unsigned mutations = 0;
};
}  // namespace
TEST(ProgressContentAnchor, ReaderContextChecksEditionAndCardWithoutReservingIdentity) {
  using namespace companion;
  Epub epub({"<html><body><p>Alpha</p></body></html>"});
  ReaderContextIdentity identities;
  auto context =
      makeUniqueNoThrow<NativeBookmarkReaderContext>(epub, identities, epub.companionIdentity, identities.identity);
  ASSERT_TRUE(context);
  ASSERT_TRUE(context->validate(epub.companionIdentity, identities.identity.storageGeneration));
  auto wrongEdition = epub.companionIdentity;
  wrongEdition[0] ^= 1;
  EXPECT_FALSE(context->validate(wrongEdition, identities.identity.storageGeneration));
  identities.card[0] ^= 1;
  EXPECT_FALSE(context->validate(epub.companionIdentity, identities.identity.storageGeneration));
  identities.card[0] ^= 1;
  epub.companionIdentityReady = false;
  EXPECT_FALSE(context->validate(epub.companionIdentity, identities.identity.storageGeneration));
  EXPECT_EQ(identities.mutations, 0u);
}
TEST(ProgressContentAnchor, ReaderContextResolvesLegacyAnchorsAndRollsBackAfterCardChange) {
  using namespace companion;
  Epub epub({"<html><body><p>Alpha</p></body></html>"});
  ReaderContextIdentity identities;
  auto context =
      makeUniqueNoThrow<NativeBookmarkReaderContext>(epub, identities, epub.companionIdentity, identities.identity);
  BookmarkEntry entry{};
  entry.xpath = "/body/DocFragment[1]/body/p[1]/text()[1].2";
  ASSERT_TRUE(NativeBookmarkReaderContext::resolveLegacy(context.get(), entry));
  EXPECT_TRUE(entry.hasVisibleTextOffset);
  EXPECT_EQ(entry.visibleTextOffset, 2u);
  context->releaseLegacyResolver();
  entry.hasVisibleTextOffset = false;
  entry.computedSpineIndex = 9;
  entry.visibleTextOffset = 99;
  epub.afterStream = +[](void* ctx) { static_cast<ReaderContextIdentity*>(ctx)->card[0] ^= 1; };
  epub.streamContext = &identities;
  EXPECT_FALSE(NativeBookmarkReaderContext::resolveLegacy(context.get(), entry));
  EXPECT_FALSE(entry.hasVisibleTextOffset);
  EXPECT_EQ(entry.computedSpineIndex, 9);
  EXPECT_EQ(entry.visibleTextOffset, 99u);
  EXPECT_EQ(identities.mutations, 0u);
}
