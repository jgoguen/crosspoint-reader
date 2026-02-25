#pragma once

#include <Epub.h>

#include <memory>
#include <string>

/**
 * Builds element-level XPath anchors for a spine item and picks the best match
 * for an intra-spine progress value.
 */
class ChapterXPathIndexer {
 public:
  /**
   * @param epub Loaded EPUB instance
   * @param spineIndex Current spine item index
   * @param intraSpineProgress Position within the spine item [0.0, 1.0]
   * @return Best matching XPath, or empty string on failure
   */
  static std::string findXPathForProgress(const std::shared_ptr<Epub>& epub, int spineIndex, float intraSpineProgress);

  /**
   * Resolve a KOReader XPath to an intra-spine progress value.
   *
   * @param epub Loaded EPUB instance
   * @param spineIndex Spine item index to parse
   * @param xpath Incoming KOReader XPath
   * @param outIntraSpineProgress Resolved position within spine [0.0, 1.0]
   * @param outExactMatch True when an exact anchor match was found
   * @return true if an exact or ancestor match was resolved
   */
  static bool findProgressForXPath(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                   float& outIntraSpineProgress, bool& outExactMatch);

  /**
   * Parse the DocFragment index from a KOReader-style XPath.
   *
   * @param xpath KOReader XPath
   * @param outSpineIndex Parsed DocFragment index (0-based)
   * @return true when DocFragment[...] is present and valid
   */
  static bool tryExtractSpineIndexFromXPath(const std::string& xpath, int& outSpineIndex);
};
