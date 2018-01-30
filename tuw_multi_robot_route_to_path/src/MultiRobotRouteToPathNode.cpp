/* Copyright (c) 2017, TU Wien
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:
      * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
      * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
      * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY TU Wien ''AS IS'' AND ANY
  EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL TU Wien BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
#include <ros/ros.h>
#include <tuw_multi_robot_route_to_path/MultiRobotRouteToPathNode.h>
#include <tf/transform_datatypes.h>



int main(int argc, char** argv)
{

    ros::init(argc, argv, "route_to_path");     /// initializes the ros node with default name
    ros::NodeHandle n;

    tuw_multi_robot_route_to_path::MultiRobotRouteToPathNode ctrl(n);
    ros::Rate r(20);

    while(ros::ok())
    {
        r.sleep();
        ros::spinOnce();
    }

    return 0;

}


namespace tuw_multi_robot_route_to_path
{
    MultiRobotRouteToPathNode::MultiRobotRouteToPathNode(ros::NodeHandle& n) :
        n_(n),
        n_param_("~"),
        robot_names_(std::vector<std::string> ( {"robot_0", "robot_1"}))
    {
        n_param_.param("robot_names", robot_names_, robot_names_);
        no_robots_ = robot_names_.size();

        ROS_INFO("Subscribing %i robots", no_robots_);

        robot_steps_.resize(no_robots_);
        pubPath_.resize(no_robots_);
        subSegPath_.resize(no_robots_);
        subOdometry_.resize(no_robots_);


        topic_path_ = "path_synced";
        n_param_.param("path_topic", topic_path_, topic_path_);

        topic_seg_path_ = "seg_path";
        n_param_.param("seg_path_topic", topic_seg_path_, topic_seg_path_);

        topic_odom_ = "odom";
        n_param_.param("odom_topic", topic_odom_, topic_odom_);

        for(int i = 0; i < no_robots_; i++)
        {
            converter_.emplace_back(no_robots_, i);
            observer_.emplace_back();
        }


        for(int i = 0; i < no_robots_; i++)
        {
            pubPath_[i] = n.advertise<nav_msgs::Path>(robot_names_[i] + "/" + topic_path_, 100);

            subOdometry_[i] = n.subscribe<nav_msgs::Odometry> (robot_names_[i] + "/" + topic_odom_, 1, boost::bind(&MultiRobotRouteToPathNode::subOdomCb, this, _1, i));
            subSegPath_[i] = n.subscribe<tuw_multi_robot_msgs::SegmentPath> (robot_names_[i] + "/" + topic_seg_path_, 1, boost::bind(&MultiRobotRouteToPathNode::subSegPathCb, this, _1, i));
        }
    }


    void MultiRobotRouteToPathNode::subOdomCb(const ros::MessageEvent< const nav_msgs::Odometry >& _event, int _topic)
    {
        const nav_msgs::Odometry_< std::allocator< void > >::ConstPtr& odom = _event.getMessage();

        Eigen::Vector2d pt(odom->pose.pose.position.x, odom->pose.pose.position.y);

        bool changed = false;
        robot_steps_[_topic] = observer_[_topic].getStep(pt, changed);

        if(changed)
        {
            for(int i = 0; i < no_robots_; i++)
            {
                std::vector<Eigen::Vector3d> newPath = converter_[i].updateSync(robot_steps_, changed);
				if(changed)
				  ROS_INFO("new path found %i %lu", i, newPath.size());
                if(changed)
                    publishPath(newPath,i);
            }
        }
    }

    void MultiRobotRouteToPathNode::publishPath(std::vector<Eigen::Vector3d> _p, int _topic)
    {
        nav_msgs::Path path;
        path.header.seq = 0;
        path.header.stamp = ros::Time::now();
        path.header.frame_id = "map";

        for(const Eigen::Vector3d & p : _p)
        {
            geometry_msgs::PoseStamped ps;
            ps.header.seq = 0;
            ps.header.stamp = ros::Time::now();
            ps.header.frame_id = "map";

            ps.pose.position.x = p[0];
            ps.pose.position.y = p[1];
			
			Eigen::Quaternion<float> q;
			q = Eigen::AngleAxisf(p[2], Eigen::Vector3f::UnitZ());
			
			ps.pose.orientation.x = q.x();
			ps.pose.orientation.y = q.y();
			ps.pose.orientation.z = q.z();
			ps.pose.orientation.w = q.w();
            path.poses.push_back(ps);
        }

		ROS_INFO("published path %i", _topic);
        pubPath_[_topic].publish(path);
    }

    void MultiRobotRouteToPathNode::subSegPathCb(const ros::MessageEvent< const tuw_multi_robot_msgs::SegmentPath >& _event, int _topic)
    {
        const tuw_multi_robot_msgs::SegmentPath_< std::allocator< void > >::ConstPtr& path = _event.getMessage();

        std::vector<SyncedPathPoint> localPath;
        std::vector<PathSegment> segPath;

        if(path->poses.size() == 0)
            return;


        for(const tuw_multi_robot_msgs::PathSegment & seg : path->poses)
        {
            SyncedPathPoint spp;
            PathSegment ps;

            ps.start[0] = seg.start.x;
            ps.start[1] = seg.start.y;

            ps.goal[0] = seg.end.x;
            ps.goal[1] = seg.end.y;

            ps.width = seg.width;               //Its the radius :D

            float angle = atan2(seg.end.y - seg.start.y, seg.end.x - seg.start.x);
            
            spp.p[0] = seg.end.x;
            spp.p[1] = seg.end.y;
            spp.p[2] = angle;

            for(const tuw_multi_robot_msgs::PathPrecondition & pc : seg.preconditions)
            {
                PathPrecondition prec;
                prec.robot_no = pc.robotId;
                prec.step = pc.stepCondition;

                spp.sync.push_back(prec);
            }

            segPath.push_back(ps);
            localPath.push_back(spp);
        }

        //Todo reset controllers with new Path
        converter_[_topic].init(localPath);
        observer_[_topic].init(segPath);
        std::fill(robot_steps_.begin(), robot_steps_.end(), 0);
		
        bool chged = false;
        std::vector<Eigen::Vector3d> newPath = converter_[_topic].updateSync(robot_steps_, chged);
		if(chged)
			  ROS_INFO("initial path found %i %lu", _topic, newPath.size());
        if(chged)
            publishPath(newPath, _topic);
    }
}


