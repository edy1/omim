#include "skin.hpp"
#include "texture.hpp"
#include "skin_page.hpp"
#include "resource_style.hpp"
#include "resource_manager.hpp"

#include "../platform/platform.hpp"

#include "../base/logging.hpp"
#include "../base/profiler.hpp"

#include "../std/iterator.hpp"
#include "../std/bind.hpp"
#include "../std/numeric.hpp"

#include "internal/opengl.hpp"

#include "../base/start_mem_debug.hpp"


namespace yg
{
  Skin::Skin(shared_ptr<ResourceManager> const & resourceManager,
             Skin::TSkinPages const & pages,
             size_t dynamicPagesCount,
             size_t textPagesCount)
    : m_pages(pages),
      m_dynamicPagesCount(dynamicPagesCount),
      m_textPagesCount(textPagesCount),
      m_staticPagesCount(pages.size()),
      m_resourceManager(resourceManager)
  {
    m_startTextPage = m_currentTextPage = m_pages.size();
    addTextPages(m_textPagesCount);

    m_startDynamicPage = m_currentDynamicPage = m_pages.size();
    addDynamicPages(m_dynamicPagesCount);
  }

  void Skin::addTextPages(int count)
  {
    m_pages.reserve(m_pages.size() + count);

    addClearPageFn(bind(&Skin::clearPageHandles, this, _1), 0);

    for (size_t i = 0; i < count; ++i)
    {
      uint8_t pageID = (uint8_t)m_pages.size();
      m_pages.push_back(make_shared_ptr(new SkinPage(m_resourceManager, pageID)));
      m_pages.back()->addOverflowFn(bind(&Skin::onTextOverflow, this, pageID), 0);
    }
  }

  void Skin::addDynamicPages(int count)
  {
    m_pages.reserve(m_pages.size() + count);

    addClearPageFn(bind(&Skin::clearPageHandles, this, _1), 0);

    for (size_t i = 0; i < count; ++i)
    {
      uint8_t pageID = (uint8_t)m_pages.size();
      m_pages.push_back(make_shared_ptr(new SkinPage(m_resourceManager, pageID)));
      m_pages.back()->addOverflowFn(bind(&Skin::onDynamicOverflow, this, pageID), 0);
    }
  }

  Skin::~Skin()
  {}

  pair<uint8_t, uint32_t> Skin::unpackID(uint32_t id) const
  {
    uint8_t pageID = (id & 0xFF000000) >> 24;
    uint32_t h = (id & 0x00FFFFFF);
    return make_pair<uint8_t, uint32_t>(pageID, h);
  }

  uint32_t Skin::packID(uint8_t pageID, uint32_t handle) const
  {
    uint32_t pageIDMask = (uint32_t)pageID << 24;
    uint32_t h = (handle & 0x00FFFFFF);
    return (uint32_t)(pageIDMask | h);
  }

  ResourceStyle const * Skin::fromID(uint32_t id)
  {
    id_pair_t p = unpackID(id);
    return m_pages[p.first]->fromID(p.second);
  }

  uint32_t Skin::mapSymbol(char const * symbolName)
  {
    for (uint8_t i = 0; i < m_pages.size(); ++i)
    {
      uint32_t res = m_pages[i]->findSymbol(symbolName);
      if (res != invalidPageHandle())
        return packID(i, res);
    }
    return invalidHandle();
  }

  uint32_t Skin::mapColor(Color const & c)
  {
    uint32_t res = invalidPageHandle();

    for (uint8_t i = 0; i < m_pages.size(); ++i)
    {
      res = m_pages[i]->findColor(c);
      if (res != invalidPageHandle())
        return packID(i, res);
    }

    if (!m_pages[m_currentDynamicPage]->hasRoom(c))
      changeCurrentDynamicPage();

    return packID(m_currentDynamicPage, m_pages[m_currentDynamicPage]->mapColor(c));
  }

  uint32_t Skin::mapPenInfo(PenInfo const & penInfo)
  {
    uint32_t res = invalidPageHandle();
    for (uint8_t i = 0; i < m_pages.size(); ++i)
    {
      res = m_pages[i]->findPenInfo(penInfo);
      if (res != invalidPageHandle())
        return packID(i, res);
    }

    if (!m_pages[m_currentDynamicPage]->hasRoom(penInfo))
      changeCurrentDynamicPage();

    return packID(m_currentDynamicPage, m_pages[m_currentDynamicPage]->mapPenInfo(penInfo));
  }

  uint32_t Skin::mapCircleInfo(CircleInfo const & circleInfo)
  {
    uint32_t res = invalidPageHandle();
    for (uint8_t i = 0; i < m_pages.size(); ++i)
    {
      res = m_pages[i]->findCircleInfo(circleInfo);
      if (res != invalidPageHandle())
        return packID(i, res);
    }

    if (!m_pages[m_currentDynamicPage]->hasRoom(circleInfo))
      changeCurrentDynamicPage();

    return packID(m_currentDynamicPage, m_pages[m_currentDynamicPage]->mapCircleInfo(circleInfo));
  }

  bool Skin::mapPenInfo(PenInfo const * penInfos, uint32_t * styleIDS, size_t count)
  {
    uint8_t savedDynamicPage = m_currentDynamicPage;

    int i = 0;

    do
    {
      styleIDS[i] = m_pages[m_currentDynamicPage]->findPenInfo(penInfos[i]);

      if ((styleIDS[i] == invalidPageHandle()) || (unpackID(styleIDS[i]).first != m_currentDynamicPage))
      {
        /// try to pack on the currentDynamicPage
        while (!m_pages[m_currentDynamicPage]->hasRoom(penInfos[i]))
        {
          /// no room - switch the page
          changeCurrentDynamicPage();
          if (savedDynamicPage == m_currentDynamicPage)
            return false; //<cycling
          /// re-start packing
          i = 0;
        }

        styleIDS[i] = packID(m_currentDynamicPage, m_pages[m_currentDynamicPage]->mapPenInfo(penInfos[i]));
      }

      ++i;
    }
    while (i != count);

    return true;
  }

  uint32_t Skin::mapGlyph(GlyphKey const & gk, bool isFixedFont)
  {
    uint32_t res = invalidPageHandle();

    for (uint8_t i = 0; i < m_pages.size(); ++i)
    {
      res = m_pages[i]->findGlyph(gk, isFixedFont);
      if (res != invalidPageHandle())
        return packID(i, res);
    }

    if (isFixedFont)
      return res;

    if (!m_pages[m_currentTextPage]->hasRoom(gk))
      changeCurrentTextPage();

    return packID(m_currentTextPage, m_pages[m_currentTextPage]->mapGlyph(gk));
  }

  Skin::TSkinPages const & Skin::pages() const
  {
    return m_pages;
  }

  void Skin::addClearPageFn(clearPageFn fn, int priority)
  {
    m_clearPageFns.push(std::pair<size_t, clearPageFn>(priority, fn));
  }

  void Skin::callClearPageFns(uint8_t pageID)
  {
    clearPageFns handlersCopy = m_clearPageFns;
    while (!handlersCopy.empty())
    {
      handlersCopy.top().second(pageID);
      handlersCopy.pop();
    }
  }

  void Skin::addOverflowFn(overflowFn fn, int priority)
  {
    m_overflowFns.push(std::pair<size_t, overflowFn>(priority, fn));
  }

  void Skin::callOverflowFns(uint8_t pageID)
  {
    overflowFns handlersCopy = m_overflowFns;
    while (!handlersCopy.empty())
    {
      handlersCopy.top().second(pageID);
      handlersCopy.pop();
    }
  }

  void Skin::clearPageHandles(uint8_t pageID)
  {
    m_pages[pageID]->clearHandles();
  }

  /// This function is set to perform as a callback on texture or handles overflow
  /// BUT! Never called on texture overflow, as this situation
  /// is explicitly checked in the mapXXX() functions.
  void Skin::onDynamicOverflow(uint8_t pageID)
  {
    LOG(LINFO, ("DynamicPage switching, pageID=", (uint32_t)pageID));
    changeCurrentDynamicPage();
  }

  void Skin::onTextOverflow(uint8_t pageID)
  {
    LOG(LINFO, ("TextPage switching, pageID=", (uint32_t)pageID));
    changeCurrentTextPage();
  }

  void Skin::changeCurrentDynamicPage()
  {
    /// flush screen(through overflowFns)
    /// callOverflowFns(m_currentDynamicPage);
    /// 1. clear currentDynamicPage
    callClearPageFns(m_currentDynamicPage);
    /// page should be frozen after flushing(activeCommands > 0)

    /// 2. choose next dynamic page
    if (m_currentDynamicPage == m_startDynamicPage + m_dynamicPagesCount - 1)
      m_currentDynamicPage = m_startDynamicPage;
    else
      ++m_currentDynamicPage;

    /// 3. clear new currentDynamicPage
    callClearPageFns(m_currentDynamicPage);
  }

  void Skin::changeCurrentTextPage()
  {
    //callOverflowFns(m_currentTextPage);
    callClearPageFns(m_currentTextPage);

    if (m_currentTextPage == m_startTextPage + m_textPagesCount - 1)
      m_currentTextPage = m_startTextPage;
    else
      ++m_currentTextPage;

    callClearPageFns(m_currentTextPage);
  }

  uint32_t Skin::invalidHandle() const
  {
    return 0xFFFFFFFF;
  }

  uint32_t Skin::invalidPageHandle() const
  {
    return 0x00FFFFFF;
  }

  void Skin::memoryWarning()
  {
  }

  void Skin::enterBackground()
  {
  }

  void Skin::enterForeground()
  {
  }
}
