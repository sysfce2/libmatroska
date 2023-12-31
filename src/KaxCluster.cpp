// Copyright © 2002-2010 Steve Lhomme.
// SPDX-License-Identifier: LGPL-2.1-or-later

/*!
  \file
  \author Steve Lhomme     <robux4 @ users.sf.net>
*/
#include "matroska/KaxCluster.h"
#include "matroska/KaxBlock.h"
#include "matroska/KaxContexts.h"
#include "matroska/KaxSegment.h"
#include "matroska/KaxDefines.h"

using namespace libebml;

// sub elements
namespace libmatroska {

KaxCluster::KaxCluster()
  :EbmlMaster(KaxCluster::ClassInfos, EBML_CLASS_SEMCONTEXT(KaxCluster))
{}

KaxCluster::KaxCluster(const KaxCluster & ElementToClone)
  :EbmlMaster(ElementToClone)
  ,bSilentTracksUsed(ElementToClone.bSilentTracksUsed)
{
  // update the parent of each children
  for (const auto& child : *this) {
    if (EbmlId(*child) == EBML_ID(KaxBlockGroup)) {
      static_cast<KaxBlockGroup   *>(child)->SetParent(*this);
    } else if (EbmlId(*child) == EBML_ID(KaxBlock)) {
      static_cast<KaxBlock        *>(child)->SetParent(*this);
    } else if (EbmlId(*child) == EBML_ID(KaxBlockVirtual)) {
      static_cast<KaxBlockVirtual *>(child)->SetParent(*this);
    }
  }
}

bool KaxCluster::AddBlockBlob(KaxBlockBlob * NewBlob)
{
  Blobs.push_back(NewBlob);
  return true;
}

bool KaxCluster::AddFrameInternal(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, const KaxBlockGroup * PastBlock, const KaxBlockGroup * ForwBlock, LacingType lacing)
{
  if (!bFirstFrameInside) {
    bFirstFrameInside = true;
    MinTimecode = MaxTimecode = timestamp;
  } else {
    if (timestamp < MinTimecode)
      MinTimecode = timestamp;
    if (timestamp > MaxTimecode)
      MaxTimecode = timestamp;
  }

  MyNewBlock = nullptr;

  if (lacing == LACING_NONE || !track.LacingEnabled()) {
    currentNewBlock = nullptr;
  }

  // force creation of a new block
  if (!currentNewBlock || static_cast<std::uint32_t>(track.TrackNumber()) != static_cast<std::uint32_t>(currentNewBlock->TrackNumber()) || PastBlock || ForwBlock) {
    KaxBlockGroup & aNewBlock = GetNewBlock();
    MyNewBlock = currentNewBlock = &aNewBlock;
  }

  if (PastBlock) {
    if (ForwBlock) {
      if (currentNewBlock->AddFrame(track, timestamp, buffer, *PastBlock, *ForwBlock, lacing)) {
        // more data are allowed in this Block
        return true;
      }

      currentNewBlock = nullptr;
      return false;
    }
    if (currentNewBlock->AddFrame(track, timestamp, buffer, *PastBlock, lacing)) {
        // more data are allowed in this Block
        return true;
      }
    currentNewBlock = nullptr;
    return false;
  }

  if (currentNewBlock->AddFrame(track, timestamp, buffer, lacing)) {
    // more data are allowed in this Block
    return true;
  }

  currentNewBlock = nullptr;
  return false;
}

bool KaxCluster::AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, LacingType lacing)
{
  assert(Blobs.empty()); // mutually exclusive for the moment
  return AddFrameInternal(track, timestamp, buffer, MyNewBlock, nullptr, nullptr, lacing);
}

bool KaxCluster::AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, const KaxBlockGroup & PastBlock, LacingType lacing)
{
  assert(Blobs.empty()); // mutually exclusive for the moment
  return AddFrameInternal(track, timestamp, buffer, MyNewBlock, &PastBlock, nullptr, lacing);
}

bool KaxCluster::AddFrame(const KaxTrackEntry & track, std::uint64_t timestamp, DataBuffer & buffer, KaxBlockGroup * & MyNewBlock, const KaxBlockGroup & PastBlock, const KaxBlockGroup & ForwBlock, LacingType lacing)
{
  assert(Blobs.empty()); // mutually exclusive for the moment
  return AddFrameInternal(track, timestamp, buffer, MyNewBlock, &PastBlock, &ForwBlock, lacing);
}

/*!
  \todo only put the Blocks written in the cue entries
*/
filepos_t KaxCluster::Render(IOCallback & output, KaxCues & CueToUpdate, ShouldWrite writeFilter)
{
  filepos_t Result = 0;

  // update the Timecode of the Cluster before writing
  auto Timecode = static_cast<KaxClusterTimecode *>(this->FindElt(EBML_INFO(KaxClusterTimecode)));
  Timecode->SetValue(GlobalTimecode() / GlobalTimecodeScale());

  if (Blobs.empty()) {
    // old-school direct KaxBlockGroup

    // SilentTracks handling
    // check the parent cluster for existing tracks and see if they are contained in this cluster or not
    if (bSilentTracksUsed) {
      auto MyTracks = static_cast<KaxTracks *>(ParentSegment->FindElt(EBML_INFO(KaxTracks)));
      for (const auto& Trk : *MyTracks) {
        if (EbmlId(*Trk) == EBML_ID(KaxTrackEntry)) {
          auto entry = static_cast<KaxTrackEntry *>(Trk);
          auto tracknum = static_cast<std::uint32_t>(entry->TrackNumber());
          auto track = std::find_if(GetElementList().begin(), GetElementList().end(), [=](EbmlElement *element)
              { return EbmlId(*element) == EBML_ID(KaxBlockGroup) && static_cast<KaxBlockGroup *>(element)->TrackNumber() == tracknum;  });
          // the track wasn't found in this cluster
          if (track == GetElementList().end()) {
            auto SilentTracks = static_cast<KaxClusterSilentTracks *>(this->FindFirstElt(EBML_INFO(KaxClusterSilentTracks)));
            assert(SilentTracks); // the flag bSilentTracksUsed should be set when creating the Cluster
            auto trackelt = static_cast<KaxClusterSilentTrackNumber *>(SilentTracks->AddNewElt(EBML_INFO(KaxClusterSilentTrackNumber)));
            trackelt->SetValue(tracknum);
          }
        }
      }
    }

    Result = EbmlMaster::Render(output, writeFilter);
    // For all Blocks add their position on the CueEntry

    for (const auto& element : GetElementList()) {
      if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
        CueToUpdate.PositionSet(*static_cast<const KaxBlockGroup *>(element));
      }
    }
  } else {
    // new school, using KaxBlockBlob
    for (const auto& blob : Blobs) {
      if (blob->IsSimpleBlock())
        PushElement( static_cast<KaxSimpleBlock&>(*blob));
      else
        PushElement( static_cast<KaxBlockGroup&>(*blob));
    }

    // SilentTracks handling
    // check the parent cluster for existing tracks and see if they are contained in this cluster or not
    if (bSilentTracksUsed) {
      auto MyTracks = static_cast<KaxTracks *>(ParentSegment->FindElt(EBML_INFO(KaxTracks)));
      for (const auto& Trk : *MyTracks) {
        if (EbmlId(*Trk) == EBML_ID(KaxTrackEntry)) {
          auto entry = static_cast<KaxTrackEntry *>(Trk);
          auto tracknum = static_cast<std::uint32_t>(entry->TrackNumber());
          auto it = std::find_if(Blobs.begin(), Blobs.end(), [tracknum](auto b){ return static_cast<KaxInternalBlock&>(*b).TrackNum() == tracknum; });

          // the track wasn't found in this cluster
          if (it == Blobs.end()) {
            auto SilentTracks = static_cast<KaxClusterSilentTracks *>(this->FindFirstElt(EBML_INFO(KaxClusterSilentTracks)));
            assert(SilentTracks); // the flag bSilentTracksUsed should be set when creating the Cluster
            auto trackelt = static_cast<KaxClusterSilentTrackNumber *>(SilentTracks->AddNewElt(EBML_INFO(KaxClusterSilentTrackNumber)));
            trackelt->SetValue(tracknum);
          }
        }
      }
    }

    Result = EbmlMaster::Render(output, writeFilter);

    // For all Blocks add their position on the CueEntry
    for (const auto& blob : Blobs)
      CueToUpdate.PositionSet(*blob);

    Blobs.clear();
  }

  return Result;
}

/*!
  \todo automatically choose valid timecode for the Cluster based on the previous cluster timecode (must be incremental)
*/
std::uint64_t KaxCluster::GlobalTimecode() const
{
  assert(bPreviousTimestampIsSet);
  std::uint64_t result = MinTimecode;

  if (result < PreviousTimestamp)
    result = PreviousTimestamp + 1;

  return result;
}

/*!
  \brief retrieve the relative
  \todo !!! We need a way to know the TimecodeScale
*/
std::int16_t KaxCluster::GetBlockLocalTimecode(std::uint64_t aGlobalTimecode) const
{
  const std::int64_t TimecodeDelay = (static_cast<std::int64_t>(aGlobalTimecode) - static_cast<std::int64_t>(GlobalTimecode())) / static_cast<std::int64_t>(GlobalTimecodeScale());
  assert(TimecodeDelay >= std::int16_t(0x8000) && TimecodeDelay <= std::int16_t(0x7FFF));
  return static_cast<std::int16_t>(TimecodeDelay);
}

std::uint64_t KaxCluster::GetBlockGlobalTimecode(std::int16_t LocalTimecode)
{
  if (!bFirstFrameInside) {
    auto Timecode = static_cast<KaxClusterTimecode *>(this->FindElt(EBML_INFO(KaxClusterTimecode)));
    assert (bFirstFrameInside); // use the InitTimecode() hack for now
    MinTimecode = MaxTimecode = PreviousTimestamp = static_cast<std::uint64_t>(*static_cast<EbmlUInteger *>(Timecode));
    bFirstFrameInside = true;
    bPreviousTimestampIsSet = true;
  }
  return static_cast<std::int64_t>(LocalTimecode * GlobalTimecodeScale()) + GlobalTimecode();
}

KaxBlockGroup & KaxCluster::GetNewBlock()
{
  auto & MyBlock = AddNewChild<KaxBlockGroup>(*this);
  MyBlock.SetParent(*this);
  return MyBlock;
}

void KaxCluster::ReleaseFrames()
{
  for (const auto& element : GetElementList()) {
    if (EbmlId(*element) == EBML_ID(KaxBlockGroup)) {
      static_cast<KaxBlockGroup *>(element)->ReleaseFrames();
    }
  }
}

std::uint64_t KaxCluster::GetPosition() const
{
  assert(ParentSegment);
  return ParentSegment->GetRelativePosition(*this);
}

} // namespace libmatroska
