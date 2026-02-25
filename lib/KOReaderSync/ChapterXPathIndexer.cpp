#include "ChapterXPathIndexer.h"

#include <Logging.h>
#include <expat.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct XPathAnchor {
  size_t textOffset = 0;
  std::string xpath;
};

struct StackNode {
  std::string tag;
  int index = 1;
  bool hasTextAnchor = false;
};

struct ParserState {
  explicit ParserState(const int spineIndex) : spineIndex(spineIndex) { siblingCounters.emplace_back(); }

  int spineIndex = 0;
  int skipDepth = -1;
  size_t totalTextBytes = 0;

  std::vector<StackNode> stack;
  std::vector<std::unordered_map<std::string, int>> siblingCounters;
  std::vector<XPathAnchor> anchors;

  std::string baseXPath() const { return "/body/DocFragment[" + std::to_string(spineIndex) + "]/body"; }

  static std::string normalizeXPath(const std::string& input) {
    if (input.empty()) {
      return "";
    }

    std::string out;
    out.reserve(input.size());
    for (char c : input) {
      const unsigned char uc = static_cast<unsigned char>(c);
      if (std::isspace(uc)) {
        continue;
      }
      out.push_back(static_cast<char>(std::tolower(uc)));
    }

    const std::string textSuffix = "/text()";
    size_t textPos = out.find(textSuffix);
    if (textPos != std::string::npos) {
      out.erase(textPos);
    }

    while (!out.empty() && out.back() == '/') {
      out.pop_back();
    }

    return out;
  }

  static std::string removeIndices(const std::string& xpath) {
    std::string out;
    out.reserve(xpath.size());

    bool inBracket = false;
    for (char c : xpath) {
      if (c == '[') {
        inBracket = true;
        continue;
      }
      if (c == ']') {
        inBracket = false;
        continue;
      }
      if (!inBracket) {
        out.push_back(c);
      }
    }
    return out;
  }

  static int pathDepth(const std::string& xpath) {
    int depth = 0;
    for (char c : xpath) {
      if (c == '/') {
        depth++;
      }
    }
    return depth;
  }

  bool pickBestAnchorByPath(const std::string& targetPath, const bool ignoreIndices, size_t& outTextOffset,
                            bool& outExact) const {
    if (targetPath.empty() || anchors.empty()) {
      return false;
    }

    const std::string normalizedTarget = ignoreIndices ? removeIndices(targetPath) : targetPath;
    std::string probe = normalizedTarget;
    bool exactProbe = true;

    while (!probe.empty()) {
      int bestDepth = -1;
      size_t bestOffset = 0;
      bool found = false;

      for (const auto& anchor : anchors) {
        const std::string anchorPath = ignoreIndices ? removeIndices(anchor.xpath) : anchor.xpath;
        if (anchorPath == probe) {
          const int depth = pathDepth(anchorPath);
          if (!found || depth > bestDepth || (depth == bestDepth && anchor.textOffset > bestOffset)) {
            found = true;
            bestDepth = depth;
            bestOffset = anchor.textOffset;
          }
        }
      }

      if (found) {
        outTextOffset = bestOffset;
        outExact = exactProbe;
        return true;
      }

      const size_t lastSlash = probe.find_last_of('/');
      if (lastSlash == std::string::npos || lastSlash == 0) {
        break;
      }
      probe.erase(lastSlash);
      exactProbe = false;
    }

    return false;
  }

  static std::string toLower(std::string value) {
    for (char& c : value) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
  }

  static bool isSkippableTag(const std::string& tag) { return tag == "head" || tag == "script" || tag == "style"; }

  static bool isWhitespaceOnly(const XML_Char* text, const int len) {
    for (int i = 0; i < len; i++) {
      if (!std::isspace(static_cast<unsigned char>(text[i]))) {
        return false;
      }
    }
    return true;
  }

  static size_t countVisibleBytes(const XML_Char* text, const int len) {
    size_t count = 0;
    for (int i = 0; i < len; i++) {
      if (!std::isspace(static_cast<unsigned char>(text[i]))) {
        count++;
      }
    }
    return count;
  }

  int bodyDepth() const {
    for (int i = static_cast<int>(stack.size()) - 1; i >= 0; i--) {
      if (stack[i].tag == "body") {
        return i;
      }
    }
    return -1;
  }

  bool insideBody() const { return bodyDepth() >= 0; }

  std::string currentXPath() const {
    const int bodyIdx = bodyDepth();
    if (bodyIdx < 0) {
      return baseXPath();
    }

    std::string xpath = baseXPath();
    for (size_t i = static_cast<size_t>(bodyIdx + 1); i < stack.size(); i++) {
      xpath += "/" + stack[i].tag + "[" + std::to_string(stack[i].index) + "]";
    }
    return xpath;
  }

  void addAnchorIfNeeded() {
    if (!insideBody() || stack.empty()) {
      return;
    }

    if (!stack.back().hasTextAnchor) {
      anchors.push_back({totalTextBytes, currentXPath()});
      stack.back().hasTextAnchor = true;
    } else if (anchors.empty() || totalTextBytes - anchors.back().textOffset >= 192) {
      const std::string xpath = currentXPath();
      if (anchors.empty() || anchors.back().xpath != xpath) {
        anchors.push_back({totalTextBytes, xpath});
      }
    }
  }

  void onStartElement(const XML_Char* rawName) {
    std::string name = toLower(rawName ? rawName : "");
    const size_t depth = stack.size();

    if (siblingCounters.size() <= depth) {
      siblingCounters.resize(depth + 1);
    }
    const int siblingIndex = ++siblingCounters[depth][name];

    stack.push_back({name, siblingIndex, false});
    siblingCounters.emplace_back();

    if (skipDepth < 0 && isSkippableTag(name)) {
      skipDepth = static_cast<int>(stack.size()) - 1;
    }
  }

  void onEndElement() {
    if (stack.empty()) {
      return;
    }

    if (skipDepth == static_cast<int>(stack.size()) - 1) {
      skipDepth = -1;
    }

    stack.pop_back();
    if (!siblingCounters.empty()) {
      siblingCounters.pop_back();
    }
  }

  void onCharacterData(const XML_Char* text, const int len) {
    if (skipDepth >= 0 || len <= 0 || !insideBody() || isWhitespaceOnly(text, len)) {
      return;
    }

    addAnchorIfNeeded();
    totalTextBytes += countVisibleBytes(text, len);
  }

  std::string chooseXPath(const float intraSpineProgress) const {
    if (anchors.empty()) {
      return baseXPath();
    }
    if (totalTextBytes == 0) {
      return anchors.front().xpath;
    }

    const float clampedProgress = std::max(0.0f, std::min(1.0f, intraSpineProgress));
    const size_t target = static_cast<size_t>(clampedProgress * static_cast<float>(totalTextBytes));

    auto it = std::lower_bound(anchors.begin(), anchors.end(), target,
                               [](const XPathAnchor& anchor, const size_t value) { return anchor.textOffset < value; });

    if (it == anchors.end()) {
      return anchors.back().xpath;
    }
    return it->xpath;
  }

  bool chooseProgressForXPath(const std::string& xpath, float& outIntraSpineProgress, bool& outExactMatch) const {
    if (anchors.empty()) {
      return false;
    }

    const std::string normalized = normalizeXPath(xpath);
    if (normalized.empty()) {
      return false;
    }

    size_t matchedOffset = 0;
    bool exact = false;
    bool matched = pickBestAnchorByPath(normalized, false, matchedOffset, exact);

    if (!matched) {
      matched = pickBestAnchorByPath(normalized, true, matchedOffset, exact);
    }

    if (!matched) {
      return false;
    }

    outExactMatch = exact;
    if (totalTextBytes == 0) {
      outIntraSpineProgress = 0.0f;
      return true;
    }

    outIntraSpineProgress = static_cast<float>(matchedOffset) / static_cast<float>(totalTextBytes);
    outIntraSpineProgress = std::max(0.0f, std::min(1.0f, outIntraSpineProgress));
    return true;
  }
};

void XMLCALL onStartElement(void* userData, const XML_Char* name, const XML_Char**) {
  auto* state = static_cast<ParserState*>(userData);
  state->onStartElement(name);
}

void XMLCALL onEndElement(void* userData, const XML_Char*) {
  auto* state = static_cast<ParserState*>(userData);
  state->onEndElement();
}

void XMLCALL onCharacterData(void* userData, const XML_Char* text, const int len) {
  auto* state = static_cast<ParserState*>(userData);
  state->onCharacterData(text, len);
}

void XMLCALL onDefaultHandlerExpand(void* userData, const XML_Char* text, const int len) {
  auto* state = static_cast<ParserState*>(userData);
  state->onCharacterData(text, len);
}

}  // namespace

std::string ChapterXPathIndexer::findXPathForProgress(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                                      const float intraSpineProgress) {
  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount()) {
    return "";
  }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    return "";
  }

  size_t chapterSize = 0;
  uint8_t* chapterBytes = epub->readItemContentsToBytes(spineItem.href, &chapterSize, false);
  if (!chapterBytes || chapterSize == 0) {
    free(chapterBytes);
    return "";
  }

  ParserState state(spineIndex);

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    free(chapterBytes);
    LOG_ERR("KOX", "Failed to allocate XML parser for spine=%d", spineIndex);
    return "";
  }

  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStartElement, onEndElement);
  XML_SetCharacterDataHandler(parser, onCharacterData);
  XML_SetDefaultHandlerExpand(parser, onDefaultHandlerExpand);

  const bool parseOk = XML_Parse(parser, reinterpret_cast<const char*>(chapterBytes), static_cast<int>(chapterSize),
                                 XML_TRUE) != XML_STATUS_ERROR;

  if (!parseOk) {
    LOG_ERR("KOX", "XPath parse failed for spine=%d at line %lu: %s", spineIndex, XML_GetCurrentLineNumber(parser),
            XML_ErrorString(XML_GetErrorCode(parser)));
  }

  XML_ParserFree(parser);
  free(chapterBytes);

  if (!parseOk) {
    return "";
  }

  return state.chooseXPath(intraSpineProgress);
}

bool ChapterXPathIndexer::findProgressForXPath(const std::shared_ptr<Epub>& epub, const int spineIndex,
                                               const std::string& xpath, float& outIntraSpineProgress,
                                               bool& outExactMatch) {
  outIntraSpineProgress = 0.0f;
  outExactMatch = false;

  if (!epub || spineIndex < 0 || spineIndex >= epub->getSpineItemsCount() || xpath.empty()) {
    return false;
  }

  const auto spineItem = epub->getSpineItem(spineIndex);
  if (spineItem.href.empty()) {
    return false;
  }

  size_t chapterSize = 0;
  uint8_t* chapterBytes = epub->readItemContentsToBytes(spineItem.href, &chapterSize, false);
  if (!chapterBytes || chapterSize == 0) {
    free(chapterBytes);
    return false;
  }

  ParserState state(spineIndex);
  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    free(chapterBytes);
    LOG_ERR("KOX", "Failed to allocate XML parser for reverse lookup spine=%d", spineIndex);
    return false;
  }

  XML_SetUserData(parser, &state);
  XML_SetElementHandler(parser, onStartElement, onEndElement);
  XML_SetCharacterDataHandler(parser, onCharacterData);
  XML_SetDefaultHandlerExpand(parser, onDefaultHandlerExpand);

  const bool parseOk = XML_Parse(parser, reinterpret_cast<const char*>(chapterBytes), static_cast<int>(chapterSize),
                                 XML_TRUE) != XML_STATUS_ERROR;

  if (!parseOk) {
    LOG_ERR("KOX", "Reverse XPath parse failed for spine=%d at line %lu: %s", spineIndex,
            XML_GetCurrentLineNumber(parser), XML_ErrorString(XML_GetErrorCode(parser)));
  }

  XML_ParserFree(parser);
  free(chapterBytes);

  if (!parseOk) {
    return false;
  }

  return state.chooseProgressForXPath(xpath, outIntraSpineProgress, outExactMatch);
}

bool ChapterXPathIndexer::tryExtractSpineIndexFromXPath(const std::string& xpath, int& outSpineIndex) {
  outSpineIndex = -1;
  if (xpath.empty()) {
    return false;
  }

  const std::string normalized = ParserState::normalizeXPath(xpath);
  const std::string key = "/docfragment[";
  const size_t pos = normalized.find(key);
  if (pos == std::string::npos) {
    return false;
  }

  const size_t start = pos + key.size();
  size_t end = start;
  while (end < normalized.size() && std::isdigit(static_cast<unsigned char>(normalized[end]))) {
    end++;
  }

  if (end == start || end >= normalized.size() || normalized[end] != ']') {
    return false;
  }

  const std::string value = normalized.substr(start, end - start);
  const long parsed = std::strtol(value.c_str(), nullptr, 10);
  if (parsed < 0 || parsed > std::numeric_limits<int>::max()) {
    return false;
  }

  outSpineIndex = static_cast<int>(parsed);
  return true;
}
