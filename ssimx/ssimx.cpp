/*
	SSIMULACRA - Structural SIMilarity Unveiling Local And Compression Related Artifacts

	Cloudinary's variant of DSSIM, based on Philipp Klaus Krause's adaptation of Rabah Mehdi's SSIM implementation,
	using ideas from Kornel Lesinski's DSSIM implementation as well as several new ideas.

	May 2016 - Feb 2017, Jon Sneyers <jon@cloudinary.com>

	Copyright 2017, Cloudinary

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.


	Changes compared to Krause's SSIM implementation:
	- Use C++ OpenCV API
	- Convert sRGB to linear RGB and then to L*a*b*, to get a perceptually more accurate color space
	- Multi-scale (6 scales)
	- Extra penalty for specific kinds of artifacts:
		- local artifacts
		- grid-like artifacts (blockiness)
		- introducing edges where the original is smooth (blockiness / color banding / ringing / mosquito noise)

	Known limitations:
	- Color profiles are ignored; input images are assumed to be sRGB.
	- Both input images need to have the same number of channels (Grayscale / RGB / RGBA)
*/

/*
	This DSSIM program has been created by Philipp Klaus Krause based on
	Rabah Mehdi's C++ implementation of SSIM (http://mehdi.rabah.free.fr/SSIM).
	Originally it has been created for the VMV '09 paper
	"ftc - floating precision texture compression" by Philipp Klaus Krause.

	The latest version of this program can probably be found somewhere at
	http://www.colecovision.eu.

	It can be compiled using g++ -I/usr/include/opencv -lcv -lhighgui dssim.cpp
	Make sure OpenCV is installed (e.g. for Debian/ubuntu: apt-get install
	libcv-dev libhighgui-dev).

	DSSIM is described in
	"Structural Similarity-Based Object Tracking in Video Sequences" by Loza et al.
	however setting all Ci to 0 as proposed there results in numerical instabilities.
	Thus this implementation used the Ci from the SSIM implementation.
	SSIM is described in
	"Image quality assessment: from error visibility to structural similarity" by Wang et al.
*/

/*
	Copyright (c) 2005, Rabah Mehdi <mehdi.rabah@gmail.com>

	Feel free to use it as you want and to drop me a mail
	if it has been useful to you. Please let me know if you enhance it.
	I'm not responsible if this program destroy your life & blablabla :)

	Copyright (c) 2009, Philipp Klaus Krause <philipp@colecovision.eu>

	Permission to use, copy, modify, and/or distribute this software for any
	purpose with or without fee is hereby granted, provided that the above
	copyright notice and this permission notice appear in all copies.

	THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
	WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
	MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
	ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
	WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
	ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
	OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#include <opencv2/opencv.hpp>
#include <avif/avif.h>
#include <stdio.h>
#include <set>

// comment this in to produce debug images that show the differences at each scale
//#define DEBUG_IMAGES 1
using namespace std;
using namespace cv;

// All of the constants below are more or less arbitrary.
// Some amount of tweaking/calibration was done, but there is certainly room for improvement.

// SSIM constants. Original C2 was 0.0009, but a smaller value seems to work slightly better.
const double C1 = 0.0001, C2 = 0.0004;

// Weight of each scale. Somewhat arbitrary.
// These are based on the values used in IW-SSIM and Kornel's DSSIM.
// It seems weird to give so little weight to the full-size scale, but then again,
// differences in more zoomed-out scales have more visual impact.
// Anyway, these weights seem to work.
// Added one more scale compared to IW-SSIM and Kornel's DSSIM.
// Weights for chroma are modified to give more weight to larger scales (similar to Kornel's subsampled chroma)
const double scale_weights[4][6] = {
	// 1:1   1:2     1:4     1:8     1:16    1:32
	{0.0448, 0.2856, 0.3001, 0.2363, 0.1333, 0.1  },
	{0.015,  0.0448, 0.2856, 0.3001, 0.3363, 0.25 },
	{0.015,  0.0448, 0.2856, 0.3001, 0.3363, 0.25 },
	{0.0448, 0.2856, 0.3001, 0.2363, 0.1333, 0.1  },
};

// higher value means more importance to chroma (weights above are multiplied by this factor for chroma and alpha)
const double chroma_weight = 0.2;

// Weights for the worst-case (minimum) score at each scale.
// Higher value means more importance to worst artifacts, lower value means more importance to average artifacts.
const double mscale_weights[4][6] = {
	// 1:4   1:8     1:16    1:32   1:64   1:128
	{0.2,    0.3,    0.25,   0.2,   0.12,  0.05},
	{0.01,   0.05,   0.2,    0.3,   0.35,  0.35},
	{0.01,   0.05,   0.2,    0.3,   0.35,  0.35},
	{0.2,    0.3,    0.25,   0.2,   0.12,  0.05},
};


// higher value means more importance to worst local artifacts
const double min_weight[4] = { 0.1,0.005,0.005,0.005 };

// higher value means more importance to artifact-edges (edges where original is smooth)
const double extra_edges_weight[4] = { 1.5, 0.1, 0.1, 0.5 };

// higher value means more importance to grid-like artifacts (blockiness)
const double worst_grid_weight[2][4] =
{ {1.0, 0.1, 0.1, 0.5},             // on ssim heatmap
  {1.0, 0.1, 0.1, 0.5} };           // on extra_edges heatmap


// Convert linear RGB to L*a*b* (all in 0..1 range)
//inline void rgb2lab(Vec3f &p){ __attribute__ ((hot));
inline void rgb2lab(Vec3d& p) {
	const double epsilon = 0.00885645167903563081f;
	const double s = 0.13793103448275862068f;
	const double k = 7.78703703703703703703f;

	// D65 adjustment included
	double fx = (p[2] * 0.43393624408206207259f + p[1] * 0.37619779063650710152f + p[0] * .18983429773803261441f);
	double fy = (p[2] * 0.2126729f + p[1] * 0.7151522f + p[0] * 0.0721750f);
	double fz = (p[2] * 0.01775381083562901744f + p[1] * 0.10945087235996326905f + p[0] * 0.87263921028466483011f);

	double X = (fx > epsilon) ? pow(fx, 1.0f / 3.0f) - s : k * fx;
	double Y = (fy > epsilon) ? pow(fy, 1.0f / 3.0f) - s : k * fy;
	double Z = (fz > epsilon) ? pow(fz, 1.0f / 3.0f) - s : k * fz;

	p[0] = Y * 1.16f;
	p[1] = (0.39181818181818181818f + 2.27272727272727272727f * (X - Y));
	p[2] = (0.49045454545454545454f + 0.90909090909090909090f * (Y - Z));
}

void grid_artifacts(Mat& errormap, unsigned int nChan, double& score, double& score_max, int twice) {
	// grid-like artifact detection
	// do the things below twice: once for the SSIM map, once for the artifact-edge map

	  // Find the 2nd percentile worst row. If the compression uses blocks, there will be artifacts around the block edges,
	  // so even with 32x32 blocks, the 2nd percentile will likely be one of the rows with block borders
	multiset<double> row_scores[4];
	for (int y = 0; y < errormap.rows; y++) {
		Mat roi = errormap(Rect(0, y, errormap.cols, 1));
		Scalar ravg = mean(roi);
		for (unsigned int i = 0; i < nChan; i++) row_scores[i].insert(ravg[i]);
	}
	for (unsigned int i = 0; i < nChan; i++) {
		int k = 0; for (const double& s : row_scores[i]) { if (k++ >= errormap.rows / 50) { score += worst_grid_weight[twice][i] * s; break; } }
		score_max += worst_grid_weight[twice][i];
	}
	// Find the 2nd percentile worst column. Same concept as above.
	multiset<double> col_scores[4];
	for (int x = 0; x < errormap.cols; x++) {
		Mat roi = errormap(Rect(x, 0, 1, errormap.rows));
		Scalar cavg = mean(roi);
		for (unsigned int i = 0; i < nChan; i++) col_scores[i].insert(cavg[i]);
	}
	for (unsigned int i = 0; i < nChan; i++) {
		int k = 0; for (const double& s : col_scores[i]) { if (k++ >= errormap.cols / 50) { score += worst_grid_weight[twice][i] * s; break; } }
		score_max += worst_grid_weight[twice][i];
	}
}

Mat readAvif(char* inputFilename) {
	avifRGBImage rgb;
	memset(&rgb, 0, sizeof(rgb));

	avifDecoder* decoder = avifDecoderCreate();

	avifResult result = avifDecoderSetIOFile(decoder, inputFilename);
	if (result != AVIF_RESULT_OK) {
		fprintf(stderr, "Cannot open file for read: %s\n", inputFilename);
		exit(-1);
	}

	result = avifDecoderParse(decoder);
	if (result != AVIF_RESULT_OK) {
		fprintf(stderr, "Failed to decode image: %s\n", avifResultToString(result));
		exit(-1);
	}

	while (avifDecoderNextImage(decoder) == AVIF_RESULT_OK) {
		avifRGBImageSetDefaults(&rgb, decoder->image);
		avifRGBImageAllocatePixels(&rgb);

		if (avifImageYUVToRGB(decoder->image, &rgb) != AVIF_RESULT_OK) {
			fprintf(stderr, "Conversion from YUV failed: %s\n", inputFilename);
			exit(-1);
		}
	}

	avifDecoderDestroy(decoder);

	return Mat(rgb.height, rgb.width, CV_8UC4, rgb.pixels);
}

int main(int argc, char** argv) {

	if (argc < 3) {
		fprintf(stderr, "Usage: %s orig_image distorted_image [difference output prefix]\n", argv[0]);
		fprintf(stderr, "Returns a value between 0 (images are identical) and 1 (images are very different)\n");
		fprintf(stderr, "If the value is above 0.1 (or so), the distortion is likely to be perceptible / annoying.\n");
		fprintf(stderr, "If the value is below 0.01 (or so), the distortion is likely to be imperceptible.\n");
		return(-1);
	}

	Scalar sC1 = { C1,C1,C1,C1 };

	Mat img1, img2;
	Mat img1_temp, img2_temp;

	// read and validate input images

	string argv1_string = string(argv[1]);
	string argv2_string = string(argv[2]);

	string extension1 = argv1_string.substr(argv1_string.find_last_of(".") + 1);
	string extension2 = argv2_string.substr(argv2_string.find_last_of(".") + 1);

	if (extension1 == "avif") cvtColor(readAvif(argv[1]), img1_temp, COLOR_RGB2BGR);
	else img1_temp = imread(argv[1], -1);

	if (extension2 == "avif") cvtColor(readAvif(argv[2]), img2_temp, COLOR_RGB2BGR);
	else img2_temp = imread(argv[2], -1);

	if (img1_temp.size() != img2_temp.size()) {
		fprintf(stderr, "Image dimensions have to be identical.\n");
		fprintf(stderr, "Image file %s is %i by %i, while\n", argv[1], img1_temp.size().width, img1_temp.size().height);
		fprintf(stderr, "image file %s is %i by %i. Can't compare.\n", argv[2], img2_temp.size().width, img2_temp.size().height);
		return -1;
	}

	if (img1_temp.cols < 8 || img1_temp.rows < 8) {
		fprintf(stderr, "Image is too small; need at least 8 rows and columns.");
		return -1;
	}

	int img1_temp_channels = img1_temp.channels();
	int img2_temp_channels = img2_temp.channels();

	if (img1_temp_channels != img2_temp_channels) {
		if (img1_temp_channels < 3 || img2_temp_channels < 3) {
			fprintf(stderr, "Image file %s has %i channels, while\n", argv[1], img1_temp_channels);
			fprintf(stderr, "image file %s has %i channels. Can't compare.\n", argv[2], img2_temp_channels);
			return -1;
		}

		if (img1_temp_channels == 3) {
			cvtColor(img1_temp, img1_temp, COLOR_RGB2RGBA);
		}
		if (img2_temp_channels == 3) {
			cvtColor(img2_temp, img2_temp, COLOR_RGB2RGBA);
		}
	}

	unsigned int nChan = img1_temp.channels();
	unsigned int pixels = img1_temp.rows * img1_temp.cols;

	if (nChan == 4) {
		// blend to a gray background to have a fair comparison of semi-transparent RGB values
		for (unsigned int i = 0; i < pixels; i++) {
			Vec4b& p = img1_temp.at<Vec4b>(i);
			p[0] = (p[3] * p[0] + (255 - p[3]) * 128) / 255;
			p[1] = (p[3] * p[1] + (255 - p[3]) * 128) / 255;
			p[2] = (p[3] * p[2] + (255 - p[3]) * 128) / 255;
		}
		for (unsigned int i = 0; i < pixels; i++) {
			Vec4b& p = img2_temp.at<Vec4b>(i);
			p[0] = (p[3] * p[0] + (255 - p[3]) * 128) / 255;
			p[1] = (p[3] * p[1] + (255 - p[3]) * 128) / 255;
			p[2] = (p[3] * p[2] + (255 - p[3]) * 128) / 255;
		}
	}

	if (nChan > 1) {
		// Create lookup table to convert 8-bit sRGB to linear RGB
		Mat sRGB_gamma_LUT(1, 256, CV_64FC1);
		for (int i = 0; i < 256; i++) {
			double c = i / 255.0;
			sRGB_gamma_LUT.at<double>(i) = (c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
		}

		// Convert from sRGB to linear RGB
		LUT(img1_temp, sRGB_gamma_LUT, img1);
		LUT(img2_temp, sRGB_gamma_LUT, img2);
	}
	else {
		img1 = Mat(img1_temp.rows, img1_temp.cols, CV_64FC1);
		img2 = Mat(img1_temp.rows, img1_temp.cols, CV_64FC1);
	}
	img1_temp.release();
	img2_temp.release();

	// Convert from linear RGB to Lab in a 0..1 range
	if (nChan == 3) {
		for (unsigned int i = 0; i < pixels; i++) rgb2lab(img1.at<Vec3d>(i));
		for (unsigned int i = 0; i < pixels; i++) rgb2lab(img2.at<Vec3d>(i));
	}
	else if (nChan == 4) {
		for (unsigned int i = 0; i < pixels; i++) { Vec3d p = { img1.at<Vec4d>(i)[0],img1.at<Vec4d>(i)[1],img1.at<Vec4d>(i)[2] }; rgb2lab(p); img1.at<Vec4d>(i)[0] = p[0]; img1.at<Vec4d>(i)[1] = p[1]; img1.at<Vec4d>(i)[2] = p[2]; }
		for (unsigned int i = 0; i < pixels; i++) { Vec3d p = { img2.at<Vec4d>(i)[0],img2.at<Vec4d>(i)[1],img2.at<Vec4d>(i)[2] }; rgb2lab(p); img2.at<Vec4d>(i)[0] = p[0]; img2.at<Vec4d>(i)[1] = p[1]; img2.at<Vec4d>(i)[2] = p[2]; }
	}
	else if (nChan == 1) {
		for (unsigned int i = 0; i < pixels; i++) { img1.at<double>(i) = img1_temp.at<uchar>(i) / 255.0; }
		for (unsigned int i = 0; i < pixels; i++) { img2.at<double>(i) = img2_temp.at<uchar>(i) / 255.0; }
	}
	else {
		fprintf(stderr, "Can only deal with Grayscale, RGB or RGBA input.\n");
		return(-1);
	}

	double score = 0, score_max = 0;

	for (int scale = 0; scale < 6; scale++) {
		Mat img1_img2, img1_sq, img2_sq, mu1, mu2, mu1_mu2, sigma1_sq, sigma2_sq, sigma12;

		if (img1.cols < 8 || img1.rows < 8) break;

		// Standard SSIM computation

		GaussianBlur(img1, mu1, Size(11, 11), 1.5);
		GaussianBlur(img2, mu2, Size(11, 11), 1.5);

		multiply(img1, img2, img1_img2, 1);
		GaussianBlur(img1_img2, sigma12, Size(11, 11), 1.5);
		img1_img2.release();
		multiply(mu1, mu2, mu1_mu2, 2);
		addWeighted(sigma12, 2, mu1_mu2, -1, C2, sigma12);
		mu1_mu2 += sC1;
		multiply(mu1_mu2, sigma12, mu1_mu2);
		sigma12.release();

		// asymmetric: penalty for introducing edges where there are none (e.g. blockiness), no penalty for smoothing away edges
		if (scale == 0) {
			Mat edgediff = max(abs(img2 - mu2) - abs(img1 - mu1), 0);   // positive if img2 has an edge where img1 is smooth

			// optional: write a nice debug image that shows the artifact edges
			if (argc > 3 && nChan > 2) {
				Mat edgediff_image;
				edgediff.convertTo(edgediff_image, CV_8UC3, 5000); // multiplying by more than 255 to make things easier to see

				for (unsigned int i = 0; i < pixels; i++) {
					if (nChan == 4) {
						Vec4b& p = edgediff_image.at<Vec4b>(i);
						p = { (uchar)(p[1] + p[2]), p[0], p[0], 255 };
					}
					if (nChan == 3) {
						Vec3b& p = edgediff_image.at<Vec3b>(i);
						p = { (uchar)(p[1] + p[2]), p[0], p[0] };
					}
				}

				imwrite(string(argv[3]) + ".edgediff.png", edgediff_image);
			}

			edgediff = Scalar(1.0, 1.0, 1.0, 1.0) - edgediff;

			Scalar avg = mean(edgediff);
			for (unsigned int i = 0; i < nChan; i++) {
				score += extra_edges_weight[i] * avg[i];
				score_max += extra_edges_weight[i];
			}
			grid_artifacts(edgediff, nChan, score, score_max, 1);
		}

		cv::pow(img1, 2, img1_sq);
		cv::pow(img2, 2, img2_sq);

		// scale down 50% in each iteration (don't need full-res img1/img2 anymore here)
		resize(img1, img1, Size(), 0.5, 0.5, INTER_AREA);
		resize(img2, img2, Size(), 0.5, 0.5, INTER_AREA);

		cv::pow(mu1, 2, mu1);
		cv::pow(mu2, 2, mu2);
		mu1 += mu2;
		mu2.release();

		GaussianBlur(img1_sq, sigma1_sq, Size(11, 11), 1.5);
		img1_sq.release();

		GaussianBlur(img2_sq, sigma2_sq, Size(11, 11), 1.5);
		img2_sq.release();
		addWeighted(sigma1_sq, 1, sigma2_sq, 1, 0, sigma1_sq);
		sigma2_sq.release();
		addWeighted(sigma1_sq, 1, mu1, -1, C2, sigma1_sq);
		mu1 += sC1;
		multiply(mu1, sigma1_sq, mu1);
		sigma1_sq.release();

		Mat& ssim_map = mu1_mu2;
		ssim_map /= mu1;
		mu1.release();

		if (scale == 0) grid_artifacts(ssim_map, nChan, score, score_max, 0);

		// optional: write a nice debug image that shows the problematic areas
		if (argc > 3 && scale == 0 && nChan > 2) {
			Mat ssim_image;
			ssim_map.convertTo(ssim_image, CV_8UC3, 255);

			for (int i = 0; i < ssim_image.rows * ssim_image.cols; i++) {
				if (nChan == 4) {
					Vec4b& p = ssim_image.at<Vec4b>(i);
					p = { (uchar)(255 - p[2]), (uchar)(255 - p[0]), (uchar)(255 - p[1]), 255 };
				}
				if (nChan == 3) {
					Vec3b& p = ssim_image.at<Vec3b>(i);
					p = { (uchar)(255 - p[2]), (uchar)(255 - p[0]), (uchar)(255 - p[1]) };
				}
			}

			imwrite(string(argv[3]) + ".ssim.png", ssim_image);
		}

		// average ssim over the entire image
		Scalar avg = mean(ssim_map);
		for (unsigned int i = 0; i < nChan; i++) {
			score += (i > 0 ? chroma_weight : 1.0) * avg[i] * scale_weights[i][scale];
			score_max += (i > 0 ? chroma_weight : 1.0) * scale_weights[i][scale];
		}

		// worst ssim in a particular 4x4 block (larger blocks are considered too because of multi-scale)
		resize(ssim_map, ssim_map, Size(), 0.25, 0.25, INTER_AREA);

		Mat ssim_map_c[4];
		split(ssim_map, ssim_map_c);
		for (unsigned int i = 0; i < nChan; i++) {
			double minVal;
			minMaxLoc(ssim_map_c[i], &minVal);
			score += min_weight[i] * minVal * mscale_weights[i][scale];
			score_max += min_weight[i] * mscale_weights[i][scale];
		}
	}

	score = score_max / score - 1;
	if (score < 0) score = 0; // should not happen
	if (score > 1) score = 1; // very different images

	fprintf(stdout, "%.8f\n", score);

	return(0);
}

