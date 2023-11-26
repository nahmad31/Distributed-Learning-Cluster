#ifndef MULTIBOX_DETECTOR_H_
#define MULTIBOX_DETECTOR_H_

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include <cmath>
#include <fstream>
#include <vector>

#include <tensorflow/cc/ops/const_op.h>
#include <tensorflow/cc/ops/image_ops.h>
#include <tensorflow/cc/ops/standard_ops.h>
#include <tensorflow/core/framework/graph.pb.h>
#include <tensorflow/core/framework/tensor.h>
#include <tensorflow/core/graph/default_device.h>
#include <tensorflow/core/graph/graph_def_builder.h>
#include <tensorflow/core/lib/core/errors.h>
#include <tensorflow/core/lib/core/stringpiece.h>
#include <tensorflow/core/lib/core/threadpool.h>
#include <tensorflow/core/lib/io/path.h>
#include <tensorflow/core/lib/strings/numbers.h>
#include <tensorflow/core/lib/strings/str_util.h>
#include <tensorflow/core/lib/strings/stringprintf.h>
#include <tensorflow/core/platform/init_main.h>
#include <tensorflow/core/platform/logging.h>
#include <tensorflow/core/platform/types.h>
#include <tensorflow/core/public/session.h>
#include <tensorflow/core/util/command_line_flags.h>

using tensorflow::string;

/**
 * Loads the graph/model for the Multibox DNN
 * @param graph_file_name - The filename of the Multibox graph/model
 * @param session - Pointer where model should be stored
*/
tensorflow::Status LoadGraphMulitbox(const string& graph_file_name, std::unique_ptr<tensorflow::Session>* session);

/**
 * Runs inference using the Multibox DNN
 * @param image - The filename of the image to run inference on
 * @param session - reference to loaded Multibox model
 * @param box_priors - filename containing means and standard deviations for all 784 possible detections 
 * @return string containing locations where humans are located in the image 
*/
string multiboxDetection(const string& image, std::unique_ptr<tensorflow::Session>& session, const string& box_priors, const string& image_out = "");

#endif // MULTIBOX_DETECTOR_H_
