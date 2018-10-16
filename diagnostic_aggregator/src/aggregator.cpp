/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/**! \author Kevin Watts */

#include <diagnostic_aggregator/aggregator.h>
#include "rclcpp/node.hpp"
#include "rclcpp/rclcpp.hpp"

using namespace std;
using namespace diagnostic_aggregator;

Aggregator::Aggregator() :
  pub_rate_(1.0),
  analyzer_group_(NULL),
  other_analyzer_(NULL),
  base_path_("")
{
  auto context = rclcpp::contexts::default_context::get_global_default_context();
  const std::vector<std::string> arguments = {};
  const std::vector<rclcpp::Parameter> initial_values = {
       rclcpp::Parameter("base_path",""),
       rclcpp::Parameter("pub_rate", "pub_rate_"),
     };
   const bool use_global_arguments = false;
   const bool use_intra_process = false;

   // ros::NodeHandle nh = ros::NodeHandle("~");
   auto nh = std::make_shared<rclcpp::Node>("agg_node", "~", context, arguments, initial_values, use_global_arguments, use_intra_process);
        n_ = std::make_shared<rclcpp::Node>();
   // nh.param(string("base_path"), base_path_, string(""));
  if (base_path_.size() > 0 && base_path_.find("/") != 0)
     base_path_ = "/" + base_path_;

   //nh.param("pub_rate", pub_rate_, pub_rate_);

  analyzer_group_ = new AnalyzerGroup();

  if (!analyzer_group_->init(base_path_, nh))
    {
     ROS_ERROR("Analyzer group for diagnostic aggregator failed to initialize!");
     }
  // Last analyzer handles remaining data
  other_analyzer_ = new OtherAnalyzer();
  other_analyzer_->init(base_path_); // This always returns true
/*      add_srv_ = n_.advertiseService("/diagnostics_agg/add_diagnostics", &Aggregator::addDiagnostics, this);
        diag_sub_ = n_.subscribe("/diagnostics", 1000, &Aggregator::diagCallback, this);
        agg_pub_ = n_.advertise<diagnostic_msgs::DiagnosticArray>("/diagnostics_agg", 1);
        toplevel_state_pub_ = n_.advertise<diagnostic_msgs::DiagnosticStatus>("/diagnostics_toplevel_state", 1);*/

   add_srv_ = n_->create_service<diagnostic_msgs::AddDiagnostics>("/diagnostics_agg/add_diagnostics", &Aggregator::addDiagnostics);
   diag_sub_ = n_->create_subscription<diagnostic_msgs::DiagnosticArray>("/diagnostics", &Aggregator::diagCallback);
   agg_pu_ = n_->create_publishe<diagnostic_msgs::DiagnosticArray>("/diagnostics_agg");
   toplevel_state_pub_ = n_->create_publishe<diagnostic_msgs::DiagnosticStatus>("/diagnostics_toplevel_state");
}

void Aggregator::checkTimestamp(const diagnostic_msgs::msg::DiagnosticArray::ConstPtr& diag_msg)
{
  if (diag_msg->header.stamp.toSec() != 0)
    return;

  string stamp_warn = "No timestamp set for diagnostic message. Message names: ";
  vector<diagnostic_msgs::msg::DiagnosticStatus>::const_iterator it;
  for (it = diag_msg->status.begin(); it != diag_msg->status.end(); ++it)
  {
    if (it != diag_msg->status.begin())
      stamp_warn += ", ";
    stamp_warn += it->name;
  }
  
  if (!ros_warnings_.count(stamp_warn))
  {
    ROS_WARN("%s", stamp_warn.c_str());
    ros_warnings_.insert(stamp_warn);
  }
}

void Aggregator::diagCallback(const diagnostic_msgs::msg::DiagnosticArray::ConstPtr& diag_msg)
{
  checkTimestamp(diag_msg);

  bool analyzed = false;
  { // lock the whole loop to ensure nothing in the analyzer group changes
    // during it.
    std::mutex::scoped_lock lock(mutex_);
    for (unsigned int j = 0; j < diag_msg->status.size(); ++j)
    {
      analyzed = false;
      std::shared_ptr<StatusItem> item(new StatusItem(&diag_msg->status[j]));

      if (analyzer_group_->match(item->getName()))
	analyzed = analyzer_group_->analyze(item);

      if (!analyzed)
	other_analyzer_->analyze(item);
    }
  }
}

Aggregator::~Aggregator()
{
  if (analyzer_group_) delete analyzer_group_;

  if (other_analyzer_) delete other_analyzer_;
}


void Aggregator::bondBroken(string bond_id, std::shared_ptr<Analyzer> analyzer)
{
  std::mutex::scoped_lock lock(mutex_); // Possibility of multiple bonds breaking at once
  ROS_WARN("Bond for namespace %s was broken", bond_id.c_str());
  std::vector<std::shared_ptr<bond::Bond> >::iterator elem;
  elem = std::find_if(bonds_.begin(), bonds_.end(), BondIDMatch(bond_id));
  if (elem == bonds_.end()){
    ROS_WARN("Broken bond tried to erase a bond which didn't exist.");
  } else {
    bonds_.erase(elem);
  }
  if (!analyzer_group_->removeAnalyzer(analyzer))
  {
    ROS_WARN("Broken bond tried to remove an analyzer which didn't exist.");
  }

  analyzer_group_->resetMatches();
}

void Aggregator::bondFormed(std::shared_ptr<Analyzer> group){
  ROS_DEBUG("Bond formed");
  std::mutex::scoped_lock lock(mutex_);
  analyzer_group_->addAnalyzer(group);
  analyzer_group_->resetMatches();
}

bool Aggregator::addDiagnostics(diagnostic_msgs::msg::AddDiagnostics::Request &req,
				diagnostic_msgs::msg::AddDiagnostics::Response &res)
{
  ROS_DEBUG("Got load request for namespace %s", req.load_namespace.c_str());
  // Don't currently support relative or private namespace definitions
  if (req.load_namespace[0] != '/')
  {
    res.message = "Requested load from non-global namespace. Private and relative namespaces are not supported.";
    res.success = false;
    return true;
  }

  std::shared_ptr<Analyzer> group = std::make_shared<AnalyzerGroup>();
  { // lock here ensures that bonds from the same namespace aren't added twice.
    // Without it, possibility of two simultaneous calls adding two objects.
    std::mutex::scoped_lock lock(mutex_);
    // rebuff attempts to add things from the same namespace twice
    if (std::find_if(bonds_.begin(), bonds_.end(), BondIDMatch(req.load_namespace)) != bonds_.end())
    {
      res.message = "Requested load from namespace " + req.load_namespace + " which is already in use";
      res.success = false;
      return true;
    }

    // Use a different topic for each bond to help control the message queue
    // length. Bond has a fixed size subscriber queue, so we can easily miss
    // bond heartbeats if there are too many bonds on the same topic.
    std::shared_ptr<bond::Bond> req_bond = std::make_shared<bond::Bond>(
      "/diagnostics_agg/bond" + req.load_namespace, req.load_namespace,
      std::function<void(void)>(std::bind(&Aggregator::bondBroken, this, req.load_namespace, group)),
      std::function<void(void)>(std::bind(&Aggregator::bondFormed, this, group))
									    );
    req_bond->start();

    bonds_.push_back(req_bond); // bond formed, keep track of it
  }

//  if (group->init(base_path_, ros::NodeHandle(req.load_namespace)))
    auto nh_temp
    if (group->init(base_path_, nh_temp))	
  {
    res.message = "Successfully initialised AnalyzerGroup. Waiting for bond to form.";
    res.success = true;
    return true;
  }
  else
  {
    res.message = "Failed to initialise AnalyzerGroup.";
    res.success = false;
    return true;
  }
}

void Aggregator::publishData()
{
  diagnostic_msgs::msg::DiagnosticArray diag_array;

  diagnostic_msgs::msg::DiagnosticStatus diag_toplevel_state;
  diag_toplevel_state.name = "toplevel_state";
  diag_toplevel_state.level = -1;
  int min_level = 255;
  
  vector<std::shared_ptr<diagnostic_msgs::msg::DiagnosticStatus> > processed;
  {
    std::mutex::scoped_lock lock(mutex_);
    processed = analyzer_group_->report();
  }
  for (unsigned int i = 0; i < processed.size(); ++i)
  {
    diag_array.status.push_back(*processed[i]);

    if (processed[i]->level > diag_toplevel_state.level)
      diag_toplevel_state.level = processed[i]->level;
    if (processed[i]->level < min_level)
      min_level = processed[i]->level;
  }

  vector<std::shared_ptr<diagnostic_msgs::msg::DiagnosticStatus> > processed_other = other_analyzer_->report();
  for (unsigned int i = 0; i < processed_other.size(); ++i)
  {
    diag_array.status.push_back(*processed_other[i]);

    if (processed_other[i]->level > diag_toplevel_state.level)
      diag_toplevel_state.level = processed_other[i]->level;
    if (processed_other[i]->level < min_level)
      min_level = processed_other[i]->level;
  }

  diag_array.header.stamp = ros::Time::now();

  agg_pub_.publish(diag_array);

  // Top level is error if we have stale items, unless all stale
  if (diag_toplevel_state.level > 2 && min_level <= 2)
    diag_toplevel_state.level = 2;

  toplevel_state_pub_.publish(diag_toplevel_state);
}
