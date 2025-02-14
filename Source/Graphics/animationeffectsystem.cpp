//-----------------------------------------------------------------------------
//           Name: animationeffectsystem.cpp
//      Developer: Wolfire Games LLC
//    Description:
//        License: Read below
//-----------------------------------------------------------------------------
//
//
//   Copyright 2022 Wolfire Games LLC
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.
//
//-----------------------------------------------------------------------------
#include "animationeffectsystem.h"

#include <Internal/common.h>
#include <Internal/filesystem.h>
#include <Internal/timer.h>

#include <TheoraPlayer/TheoraVideoManager.h>
#include <TheoraPlayer/TheoraDataSource.h>
#include <TheoraPlayer/TheoraVideoFrame.h>

#include <XML/xml_helper.h>
#include <Graphics/textures.h>
#include <Logging/logdata.h>

#include <tinyxml.h>

#include <cstdio>

AnimationEffectSystem::AnimationEffectSystem()
{
    mgr = new TheoraVideoManager();
}

AnimationEffectSystem::~AnimationEffectSystem()
{
    if( mgr )
        delete mgr;
    mgr = NULL;
}

void AnimationEffectSystem::Update(float timestep)
{
    mgr->update(timestep);
}

void AnimationEffectSystem::Dispose()
{
    if( mgr )
        delete mgr;
    mgr = NULL;
}
