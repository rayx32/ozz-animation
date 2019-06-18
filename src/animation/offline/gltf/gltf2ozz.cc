/*
 *
 * Ozz glTF importer
 * Author: Alexander Dzhoganov
 * Licensed under the MIT License
 * 
 */

#include <cassert>
#include <unordered_map>
#include <set>

#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE       //
#define TINYGLTF_NO_STB_IMAGE_WRITE // skip loading the image libraries
#define TINYGLTF_NO_EXTERNAL_IMAGE  //

#include "ozz/base/log.h"
#include "ozz/animation/offline/tools/import2ozz.h"
#include "ozz/animation/offline/gltf/tiny_gltf.h"
#include "ozz/animation/runtime/skeleton.h"

using namespace ozz;
using namespace ozz::animation::offline;

using string = std::string;
template <typename T> using vector = std::vector<T>;
template <typename T> using set = std::set<T>;
template <typename K, typename V> using unordered_map = std::unordered_map<K, V>;

using RawSkeleton = ozz::animation::offline::RawSkeleton;
using Skeleton = ozz::animation::Skeleton;

class GltfImporter : public animation::offline::OzzImporter
{
  public:
  GltfImporter()
  {
    // we don't care about image data but we have to provide this callback because we're not loading the stb library
    m_loader.SetImageLoader(&GltfImporter::LoadImageData, 0);
  }

  private:
  bool Load(const char* filename) override
  {
    string ext = GetFileExtension(filename);
    bool success = false;

    string errors;
    string warnings;

    // try to guess whether the input is a gltf json or a glb binary based on the file extension
    if (ext == "glb")
    {
      success = m_loader.LoadBinaryFromFile(&m_model, &errors, &warnings, filename);
    }
    else
    {
      if (ext != "gltf")
        log::Log() << "Warning: Unknown file extension '" << ext << "', assuming a JSON-formatted gltf.\n";

      success = m_loader.LoadASCIIFromFile(&m_model, &errors, &warnings, filename);
    }

    // print any errors or warnings emitted by the loader
    if (!warnings.empty())
      log::Log() << "Warning: " << warnings << "\n";

    if (!errors.empty())
      log::Err() << "Error: " << errors << "\n";

    if (success)
      log::Log() << "glTF parsed successfully.\n";
    
    return success;
  }

  bool Import(RawSkeleton* skeleton, const NodeType& types) override
  {
    if (m_model.scenes.empty())
    {
      log::Err() << "Error: No scenes found, bailing out.\n";
      return false;
    }

    if (m_model.skins.empty())
    {
      log::Err() << "Error: No skins found, bailing out.\n";
      return false;
    }

    // if no default scene has been set then take the first one
    // spec does not disallow gltfs without a default scene
    // but it makes more sense to keep going instead of throwing an error here
    int defaultScene = m_model.defaultScene;
    if (defaultScene == -1)
      defaultScene = 0;

    tinygltf::Scene& scene = m_model.scenes[defaultScene];
    log::Log() << "Importing from scene #" << defaultScene << " (" << scene.name << ").\n";

    if (scene.nodes.empty())
    {
      log::Err() << "Error: Scene has no nodes, bailing out.\n";
      return false;
    }

    // get all the skins belonging to this scene
    auto skins = GetSkinsForScene(scene);
    if (skins.empty())
    {
      log::Err() << "Error: No skins exist in the scene, bailing out.\n";
      return false;
    }

    // first find the skeleton roots for each skin
    set<int> rootJoints;
    for (auto& skin : skins)
    {
      int rootJointIndex = FindSkinRootJointIndex(skin);
      if (rootJointIndex == -1)
        continue;

      rootJoints.insert(rootJointIndex);
    }

    // traverse the scene graph and record all joints starting from the roots
    for (int rootJointIndex : rootJoints)
    {
      RawSkeleton::Joint rootJoint;

      auto& rootBone = m_model.nodes[rootJointIndex];
      rootJoint.name = CreateJointName(rootJointIndex).c_str();

      if (!CreateNodeTransform(rootBone, rootJoint.transform))
        return false;

      if (!ImportChildren(rootBone, rootJoint))
        return false;

      skeleton->roots.push_back(std::move(rootJoint));
    }

    log::Log() << "Printing joint hierarchy:\n";
    for (auto& root : skeleton->roots)
      PrintSkeletonInfo(root);

    if (!skeleton->Validate())
    {
      log::Err() << "Error: Output skeleton failed validation.\n"
        "This is likely a bug.\n";
      return false;
    }

    return true;
  }

  // creates a unique name for each joint
  // ozz requires all joint names to be non-empty and unique
  string CreateJointName(int nodeIndex)
  {
    static unordered_map<string, int> existingNames;
    auto& node = m_model.nodes[nodeIndex];

    string name = node.name;
    if (name.length() == 0)
    {
      std::stringstream s;
      s << "gltf_node_" << nodeIndex;
      name = s.str();

      log::Log() << "Warning: Joint at node #" << nodeIndex << " has no name.\n"
        "Setting name to '" << name << "'.\n";
    }

    auto it = existingNames.find(name);
    if (it != existingNames.end())
    {
      std::stringstream s;
      s << name << "_" << nodeIndex;
      name = s.str();

      log::Log() << "Warning: Joint at node #" << nodeIndex << " has the same name as node #" << it->second << "\n"
        << "This is unsupported by ozz and the joint will be renamed to '" << name << "'.";
    }

    existingNames.insert(std::make_pair(name, nodeIndex));
    m_nodeNames.insert(std::make_pair(nodeIndex, name));
    return name;
  }

  // given a skin find which of its joints is the skeleton root and return it
  // returns -1 if the skin has no associated joints
  int FindSkinRootJointIndex(const tinygltf::Skin& skin)
  {
    if (skin.joints.empty())
      return -1;

    unordered_map<int, int> parents;
    for (int nodeIndex : skin.joints)
      for (int childIndex : m_model.nodes[nodeIndex].children)
        parents.insert(std::make_pair(childIndex, nodeIndex));

    int rootBoneIndex = skin.joints[0];
    while (parents.find(rootBoneIndex) != parents.end())
      rootBoneIndex = parents[rootBoneIndex];

    return rootBoneIndex;
  }

  // recursively import a node's children
  bool ImportChildren(const tinygltf::Node& node, RawSkeleton::Joint& parent)
  {
    for (int childIndex : node.children)
    {
      tinygltf::Node& childNode = m_model.nodes[childIndex];

      RawSkeleton::Joint joint;
      joint.name = CreateJointName(childIndex).c_str();
      if (!CreateNodeTransform(childNode, joint.transform))
        return false;

      if (!ImportChildren(childNode, joint))
        return false;

      parent.children.push_back(std::move(joint));
    }

    return true;
  }

  // returns all animations in the gltf
  AnimationNames GetAnimationNames() override
  {
    AnimationNames animNames;

    for (auto& animation : m_model.animations)
    {
      if (animation.name.length() == 0)
      {
        log::Log() << "Warning: Found an animation without a name. All animations must have valid and unique names.\n"
          "The animation will be skipped.\n";
        continue;
      }

      animNames.push_back(animation.name.c_str());
    }

    return animNames;
  }

  bool Import(const char* animationName, const Skeleton& skeleton, float samplingRate, RawAnimation* animation) override
  {
    if (samplingRate == 0.0f)
    {
      samplingRate = 60.0f;

      static bool samplingRateWarn = false;
      if (!samplingRateWarn)
      {
        log::Log() << "Warning: The animation sampling rate is set to 0 (automatic) but glTF does not carry scene frame rate information.\n"
          "Assuming a sampling rate of 60hz.\n";

        samplingRateWarn = true;
      }
    }

    // find the corresponding gltf animation
    auto animationIt = std::find_if(begin(m_model.animations), end(m_model.animations),
      [animationName](const tinygltf::Animation& animation) { return animation.name == animationName; });

    // this shouldn't be possible but check anyway
    if (animationIt == end(m_model.animations))
    {
      log::Err() << "Error: Animation '" << animationName << "' requested but not found in glTF.\n";
      return false;
    }

    auto& gltfAnimation = *animationIt;

    animation->name = gltfAnimation.name.c_str();

    // animation duration is determined during sampling from the duration of the longest channel
    animation->duration = 0.0f;

    int numJoints = skeleton.num_joints();
    animation->tracks.resize(numJoints);

    // gltf stores animations by splitting them in channels
    // where each channel targets a node's property i.e. translation, rotation or scale
    // ozz expects animations to be stored per joint
    // we create a map where we record the associated channels for each joint
    unordered_map<string, vector<tinygltf::AnimationChannel>> channelsPerJoint;
    for (auto& channel : gltfAnimation.channels)
    {
      if (channel.target_node == -1)
        continue;
      
      auto& targetNode = m_model.nodes[channel.target_node];
      channelsPerJoint[targetNode.name].push_back(channel);
    }

    auto jointNames = skeleton.joint_names();

    // for each joint get all its associated channels, sample them and record the samples in the joint track
    for (int i = 0; i < numJoints; i++)
    {
      auto& channels = channelsPerJoint[jointNames[i]];
      auto& track = animation->tracks[i];

      for (auto& channel : channels)
      {
        auto& sampler = gltfAnimation.samplers[channel.sampler];
        if (!SampleAnimationChannel(sampler, channel.target_path, animation->duration, track, samplingRate))
          return false;
      }

      auto node = FindNodeByName(jointNames[i]);
      assert(node != nullptr);

      // pad the bind pose transform for any joints which do not have an associated channel for this animation
      if (track.translations.empty())
        track.translations.push_back(CreateTranslationBindPoseKey(*node));
      if (track.rotations.empty())
        track.rotations.push_back(CreateRotationBindPoseKey(*node));
      if (track.scales.empty())
        track.scales.push_back(CreateScaleBindPoseKey(*node));
    }

    log::Log() << "Processed animation '" << animation->name <<
      "' (tracks: " << animation->tracks.size() << ", duration: " << animation->duration << "s).\n";
    
    if (!animation->Validate())
    {
      log::Err() << "Error: Animation '" << animation->name << "' failed validation.\n";
      return false;
    }

    return true;
  }

  bool SampleAnimationChannel(const tinygltf::AnimationSampler& sampler, const string& targetPath,
    float& outDuration, RawAnimation::JointTrack& track, int samplingRate)
  {
    auto& input = m_model.accessors[sampler.input];
    assert(input.maxValues.size() == 1);

    // the max[0] property of the input accessor is the animation duration
    // this is required to be present by the spec:
    // "Animation Sampler's input accessor must have min and max properties defined."
    float duration = input.maxValues[0];

    // if this channel's duration is larger than the animation's duration
    // then increase the animation duration to match
    if (duration > outDuration)
      outDuration = duration;

    assert(input.type == TINYGLTF_TYPE_SCALAR);
    auto& output = m_model.accessors[sampler.output];
    assert(output.type == TINYGLTF_TYPE_VEC3 || output.type == TINYGLTF_TYPE_VEC4);

    float* timestamps = BufferView<float>(input);
    if (timestamps == nullptr)
      return false;

    if (sampler.interpolation.empty() || sampler.interpolation == "LINEAR")
    {
      assert(input.count == output.count);

      if (targetPath == "translation")
        return SampleLinearChannel(output, timestamps, track.translations);
      else if (targetPath == "rotation")
        return SampleLinearChannel(output, timestamps, track.rotations);
      else if (targetPath == "scale")
        return SampleLinearChannel(output, timestamps, track.scales);

      log::Err() << "Invalid or unknown channel target path '" << targetPath << "'.\n";
      return false;
    }
    else if (sampler.interpolation == "STEP")
    {
      assert(input.count == output.count);

      if (targetPath == "translation")
        return SampleStepChannel(output, timestamps, track.translations);
      else if (targetPath == "rotation")
        return SampleStepChannel(output, timestamps, track.rotations);
      else if (targetPath == "scale")
        return SampleStepChannel(output, timestamps, track.scales);

      log::Err() << "Invalid or unknown channel target path '" << targetPath << "'.\n";
      return false;
    }
    else if (sampler.interpolation == "CUBICSPLINE")
    {
      assert(input.count * 3 == output.count);

      if (targetPath == "translation")
      {
        return SampleCubicSplineChannel(output, timestamps, track.translations, samplingRate, duration);
      }
      else if (targetPath == "rotation")
      {
        if (!SampleCubicSplineChannel(output, timestamps, track.rotations, samplingRate, duration))
          return false;

        // normalize all resulting quaternions per spec
        for (auto& key : track.rotations)
          key.value = math::Normalize(key.value);

        return true;
      }
      else if (targetPath == "scale")
      {
        return SampleCubicSplineChannel(output, timestamps, track.scales, samplingRate, duration);
      }

      log::Err() << "Invalid or unknown channel target path '" << targetPath << "'.\n";
      return false;
    }

    log::Err() << "Invalid or unknown interpolation type '" << sampler.interpolation << "'.\n";
    return false;
  }

  // samples a linear animation channel
  // there is an exact mapping between gltf and ozz keyframes so we just copy everything over
  template <typename KeyType>
  bool SampleLinearChannel(const tinygltf::Accessor& output, float* timestamps,
    std::vector<KeyType, ozz::StdAllocator<KeyType>>& keyframes)
  {
    using ValueType = typename KeyType::Value;
    ValueType* values = BufferView<ValueType>(output);
    if (values == nullptr)
      return false;

    keyframes.resize(output.count);

    for (size_t i = 0u; i < output.count; i++)
    {
      KeyType& key = keyframes[i];
      key.time = timestamps[i];
      key.value = values[i];
    }

    return true;
  }

  // samples a step animation channel
  // there are twice as many ozz keyframes as gltf keyframes
  template <typename KeyType>
  bool SampleStepChannel(const tinygltf::Accessor& output, float* timestamps,
    std::vector<KeyType, ozz::StdAllocator<KeyType>>& keyframes)
  {
    using ValueType = typename KeyType::Value;
    ValueType* values = BufferView<ValueType>(output);
    if (values == nullptr)
      return false;

    size_t numKeyframes = output.count * 2;
    keyframes.resize(numKeyframes);

    const float eps = 1e-6f;

    for (size_t i = 0u; i < output.count; i++)
    {
      KeyType& key = keyframes[i * 2];
      key.time = timestamps[i];
      key.value = values[i];

      if (i < output.count - 1)
      {
        KeyType& nextKey = keyframes[i * 2 + 1];
        nextKey.time = timestamps[i + 1] - eps;
        nextKey.value = values[i];
      }
    }

    return true;
  }

  // samples a cubic-spline channel
  // the number of keyframes is determined from the animation duration and given sample rate
  template <typename KeyType>
  bool SampleCubicSplineChannel(const tinygltf::Accessor& output, float* timestamps,
    std::vector<KeyType, ozz::StdAllocator<KeyType>>& keyframes, float samplingRate, float duration)
  {
    using ValueType = typename KeyType::Value;
    ValueType* values = BufferView<ValueType>(output);
    if (values == nullptr)
      return false;

    assert(output.count % 3 == 0);
    size_t numKeyframes = output.count / 3;

    keyframes.resize(floor(duration * samplingRate) + 1);
    size_t currentKey = 0u;

    for (size_t i = 0u; i < keyframes.size(); i++)
    {
      float time = (float)i / samplingRate;
      while (timestamps[currentKey] > time && currentKey < numKeyframes - 1)
        currentKey++;

      float currentTime = timestamps[currentKey];  // current keyframe time
      float nextTime = timestamps[currentKey + 1]; // next keyframe time

      float t = (time - currentTime) / (nextTime - currentTime);
      ValueType& p0 = values[currentKey * 3 + 1];
      ValueType m0 = values[currentKey * 3 + 2] * (nextTime - currentTime);
      ValueType& p1 = values[(currentKey + 1) * 3 + 1];
      ValueType m1 = values[(currentKey + 1) * 3] * (nextTime - currentTime);

      KeyType& key = keyframes[i];
      key.time = time;
      key.value = SampleHermiteSpline(t, p0, m0, p1, m1);
    }

    return true;
  }
  
  // samples a hermite spline in the form
  // p(t) = (2t^3 - 3t^2 + 1)p0 + (t^3 - 2t^2 + t)m0 + (-2t^3 + 3t^2)p1 + (t^3 - t^2)m1
  // where
  // t is a value between 0 and 1
  // p0 is the starting point at t = 0
  // m0 is the scaled starting tangent at t = 0
  // p1 is the ending point at t = 1
  // m1 is the scaled ending tangent at t = 1
  // p(t) is the resulting point value
  template <typename T>
  T SampleHermiteSpline(float t, const T& p0, const T& m0, const T& p1, const T& m1)
  {
    float t2 = t * t;
    float t3 = t2 * t;

    // a = 2t^3 - 3t^2 + 1
    float a = 2.0f * t3 - 3.0f * t2 + 1.0f;
    // b = t^3 - 2t^2 + t
    float b = t3 - 2.0f * t2 + t;
    // c = -2t^3 + 3t^2
    float c = -2.0f * t3 + 3.0f * t2;
    // d = t^3 - t^2
    float d = t3 - t2;

    // p(t) = a * p0 + b * m0 + c * p1 + d * m1
    T pt = p0 * a + m0 * b + p1 * c + m1 * d;
    return pt;
  }

  // create the default transform for a gltf node
  bool CreateNodeTransform(const tinygltf::Node& node, math::Transform& transform)
  {
    if (node.matrix.size() != 0)
    {
      // For animated nodes matrix should never be set
      // From the spec: "When a node is targeted for animation (referenced by an animation.channel.target),
      // only TRS properties may be present; matrix will not be present."
      log::Err() << "Error: Node '" << node.name << "' transformation matrix is not empty.\n"
        "This is disallowed by the glTF spec as this node is an animation target.\n";
      return false;
    }

    transform = math::Transform::identity();

    if (!node.translation.empty())
      transform.translation = math::Float3(node.translation[0], node.translation[1], node.translation[2]);

    if (!node.rotation.empty())
      transform.rotation = math::Quaternion(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);

    if (!node.scale.empty())
      transform.scale = math::Float3(node.scale[0], node.scale[1], node.scale[2]);

    return true;
  }

  RawAnimation::TranslationKey CreateTranslationBindPoseKey(const tinygltf::Node& node)
  {
    RawAnimation::TranslationKey key;
    key.time = 0.0f;
    key.value = math::Float3::zero();

    if (!node.translation.empty())
      key.value = math::Float3(node.translation[0], node.translation[1], node.translation[2]);

    return key;
  }

  RawAnimation::RotationKey CreateRotationBindPoseKey(const tinygltf::Node& node)
  {
    RawAnimation::RotationKey key;
    key.time = 0.0f;
    key.value = math::Quaternion::identity();
    
    if (!node.rotation.empty())
      key.value = math::Quaternion(node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);
    
    return key;
  }

  RawAnimation::ScaleKey CreateScaleBindPoseKey(const tinygltf::Node& node)
  {
    RawAnimation::ScaleKey key;
    key.time = 0.0f;
    key.value = math::Float3::one();
    
    if (!node.scale.empty())
      key.value = math::Float3(node.scale[0], node.scale[1], node.scale[2]);

    return key;
  }

  // returns all skins belonging to a given gltf scene
  vector<tinygltf::Skin> GetSkinsForScene(const tinygltf::Scene& scene) const
  {
    set<int> open;
    set<int> found;

    for (int nodeIndex : scene.nodes)
      open.insert(nodeIndex);

    while (!open.empty())
    {
      int nodeIndex = *open.begin();
      found.insert(nodeIndex);
      open.erase(nodeIndex);

      auto& node = m_model.nodes[nodeIndex];
      for (int childIndex : node.children)
        open.insert(childIndex);
    }

    vector<tinygltf::Skin> skins;
    for (auto& skin : m_model.skins)
      if (!skin.joints.empty() && found.find(skin.joints[0]) != found.end())
        skins.push_back(skin);

    return skins;
  }

  tinygltf::Node* FindNodeByName(const string& name)
  {
    for (size_t nodeIndex = 0u; nodeIndex < m_model.nodes.size(); nodeIndex++)
    {
      auto it = m_nodeNames.find(nodeIndex);
      if (it == m_nodeNames.end())  
        continue;

      if (it->second == name)
        return &m_model.nodes[nodeIndex];
    }

    return nullptr;
  }

  // returns the address of a gltf buffer given an accessor
  // performs basic checks to ensure the data is in the correct format
  template <typename T>
  T* BufferView(const tinygltf::Accessor& accessor)
  {
    int32_t componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
    int32_t elementSize = componentSize * tinygltf::GetTypeSizeInBytes(accessor.type);
    if (elementSize != sizeof(T))
    {
      log::Err{} << "Invalid buffer view access. Expected element size '" << sizeof(T) << " got " << elementSize << "instead.\n";
      return nullptr;
    }

    auto& bufferView = m_model.bufferViews[accessor.bufferView];
    auto& buffer = m_model.buffers[bufferView.buffer];
    return (T*)(buffer.data.data() + bufferView.byteOffset + accessor.byteOffset);
  }

  void PrintSkeletonInfo(const RawSkeleton::Joint& joint, int ident = 0)
  {
    for (int i = 0; i < ident; i++)
      log::Log() << " ";
    log::Log() << joint.name << "\n";

    for (auto& child : joint.children)
      PrintSkeletonInfo(child, ident + 2);
  }

  string GetFileExtension(const string& path)
  {
    if(path.find_last_of(".") != string::npos)
      return path.substr(path.find_last_of(".")+1);
    return "";
  }

  // no support for user-defined tracks
  NodeProperties GetNodeProperties(const char*) override { return NodeProperties(); }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float, RawFloatTrack*)  override { return false; }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float, RawFloat2Track*) override { return false; }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float, RawFloat3Track*) override { return false; }
  bool Import(const char*, const char*, const char*, NodeProperty::Type, float, RawFloat4Track*) override { return false; }

  static bool LoadImageData(tinygltf::Image*, const int, string*,
    string*, int, int, const unsigned char*, int, void*)
  {
    return true;
  }
  
  tinygltf::TinyGLTF m_loader;
  tinygltf::Model m_model;

  unordered_map<int, string> m_nodeNames;
};

int main(int argc, const char** argv)
{
  return GltfImporter()(argc, argv);
}