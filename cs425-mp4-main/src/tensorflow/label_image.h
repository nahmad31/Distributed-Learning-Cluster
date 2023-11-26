#ifndef LABEL_IMAGE_H_
#define LABEL_IMAGE_H_

#include <fstream>
#include <utility>
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
#include <tensorflow/core/lib/strings/str_util.h>
#include <tensorflow/core/lib/strings/stringprintf.h>
#include <tensorflow/core/platform/env.h>
#include <tensorflow/core/platform/init_main.h>
#include <tensorflow/core/platform/logging.h>
#include <tensorflow/core/platform/types.h>
#include <tensorflow/core/public/session.h>
#include <tensorflow/core/util/command_line_flags.h>

using tensorflow::string;

/**
 * Loads the graph/model for the Inception DNN
 * @param graph_file_name - The filename of the Inception graph/model
 * @param session - Pointer where model should be stored
*/
tensorflow::Status LoadGraphLabel(const string& graph_file_name, std::unique_ptr<tensorflow::Session>* session);

/**
 * Runs inference using the Inception DNN
 * @param image - The filename of the image to run inference on
 * @param session - reference to loaded Inception model
 * @param labels - filename of valid labels the model can use to label outcomes
 * @return string containing what the model predicted the image to be  
*/
string labelImage(const string& image, std::unique_ptr<tensorflow::Session>& session, const string& labels);

#endif // LABEL_IMAGE_H_
