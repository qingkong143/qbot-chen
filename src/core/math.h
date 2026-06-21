#pragma once

#include <vector>
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <stdexcept>

template<typename T = double>
class Matrix {
private:
    int rows, cols;
    std::vector<std::vector<T>> data;

public:
    // 构造函数
    Matrix(int r, int c, T init = T()) : rows(r), cols(c) {
        data.assign(r, std::vector<T>(c, init));
    }

    // 通过二维数组初始化
    Matrix(const std::vector<std::vector<T>>& mat) {
        rows = mat.size();
        cols = rows ? mat[0].size() : 0;
        data = mat;
    }

    // 访问元素
    T& at(int i, int j) { return data[i][j]; }
    const T& at(int i, int j) const { return data[i][j]; }

    // 获取尺寸
    int getRows() const { return rows; }
    int getCols() const { return cols; }

    // 输出矩阵
    void print() const {
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < cols; ++j)
                std::cout << data[i][j] << "\t";
            std::cout << std::endl;
        }
    }
    // 初等行变换：交换两行
    void swapRows(int r1, int r2) {
        if (r1 < 0 || r1 >= rows || r2 < 0 || r2 >= rows)
            throw std::out_of_range("swapRows: row index out of range");
        std::swap(data[r1], data[r2]);
    }

    // 初等列变换：交换两列
    void swapCols(int c1, int c2) {
        if (c1 < 0 || c1 >= cols || c2 < 0 || c2 >= cols)
            throw std::out_of_range("swapCols: col index out of range");
        for (int i = 0; i < rows; ++i)
            std::swap(data[i][c1], data[i][c2]);
    }

    // 初等行变换：某行乘以非零常数
    void multiplyRow(int r, T factor) {
        if (r < 0 || r >= rows)
            throw std::out_of_range("multiplyRow: row index out of range");
        if (std::abs(factor) < 1e-12)
            throw std::invalid_argument("multiplyRow: factor must be non-zero");
        for (int j = 0; j < cols; ++j)
            data[r][j] *= factor;
    }

    // 初等行变换：把某一行的倍数加到另一行
    void addRowMultiple(int srcRow, int dstRow, T factor) {
        if (srcRow < 0 || srcRow >= rows || dstRow < 0 || dstRow >= rows)
            throw std::out_of_range("addRowMultiple: row index out of range");
        for (int j = 0; j < cols; ++j)
            data[dstRow][j] += factor * data[srcRow][j];
    }

    // 初等列变换：把某一列的倍数加到另一列
    void addColMultiple(int srcCol, int dstCol, T factor) {
        if (srcCol < 0 || srcCol >= cols || dstCol < 0 || dstCol >= cols)
            throw std::out_of_range("addColMultiple: col index out of range");
        for (int i = 0; i < rows; ++i)
            data[i][dstCol] += factor * data[i][srcCol];
    }

    // 将矩阵转为可读的字符串（矩阵形式）
    std::string toString() const {
        std::ostringstream oss;
        for (int i = 0; i < rows; ++i) {
            oss << "[";
            for (int j = 0; j < cols; ++j) {
                if (j > 0) oss << ", ";
                oss << data[i][j];
            }
            oss << "]";
            if (i < rows - 1) oss << "\n";
        }
        return oss.str();
    }

    // 计算行列式（使用高斯消元法）
    T determinant() const {
        if (rows != cols)
            throw std::invalid_argument("determinant: matrix must be square");
        Matrix<T> tmp = *this;
        T det = 1;
        int n = rows;
        for (int i = 0; i < n; ++i) {
            // 寻找主元
            int pivot = i;
            for (int k = i + 1; k < n; ++k) {
                if (std::abs(tmp.at(k, i)) > std::abs(tmp.at(pivot, i)))
                    pivot = k;
            }
            if (std::abs(tmp.at(pivot, i)) < 1e-12) return 0;
            if (pivot != i) {
                tmp.swapRows(i, pivot);
                det = -det;
            }
            det *= tmp.at(i, i);
            for (int k = i + 1; k < n; ++k) {
                T factor = tmp.at(k, i) / tmp.at(i, i);
                for (int j = i; j < n; ++j)
                    tmp.at(k, j) -= factor * tmp.at(i, j);
            }
        }
        return det;
    }

    // 检测矩阵是否奇异
    bool isSingular() const {
        if (rows != cols) return false; // 非方阵不判定
        return std::abs(determinant()) < 1e-12;
    }

    Matrix<T> toDiagonal(bool normalize = false) {
        Matrix<T> result = *this;   // 复制一份进行操作
        int r = 0, c = 0;
        int m = result.getRows(), n = result.getCols();
        while (r < m && c < n) {
            // 1. 寻找非零主元
            int pivot_row = -1, pivot_col = -1;
            for (int i = r; i < m && pivot_row == -1; ++i) {
                for (int j = c; j < n && pivot_col == -1; ++j) {
                    if (std::abs(result.at(i, j)) > 1e-12) {
                        pivot_row = i;
                        pivot_col = j;
                        break;
                    }
                }
            }
            if (pivot_row == -1) break; // 剩余全为零

            // 2. 交换行和列，将主元移动到 (r, c)
            if (pivot_row != r)
                result.swapRows(pivot_row, r);
            if (pivot_col != c)
                result.swapCols(pivot_col, c);

            T pivot = result.at(r, c);

            // 3. 用主元消去同一列其他行的元素（行变换）
            for (int i = 0; i < m; ++i) {
                if (i != r && std::abs(result.at(i, c)) > 1e-12) {
                    T factor = result.at(i, c) / pivot;
                    result.addRowMultiple(r, i, -factor);
                }
            }

            // 4. 用主元消去同一行其他列的元素（列变换）
            for (int j = 0; j < n; ++j) {
                if (j != c && std::abs(result.at(r, j)) > 1e-12) {
                    T factor = result.at(r, j) / pivot;
                    result.addColMultiple(c, j, -factor);
                }
            }

            // 5. 可选：归一化当前主元为1
            if (normalize && std::abs(pivot - 1.0) > 1e-12) {
                result.multiplyRow(r, 1.0 / pivot);
                // 注意：这会影响后续列消元，但因为其他列已被消为0，故可单独归一化
                // 如果希望严格对角矩阵，可以再列乘，但不必要，因为对角线外的值已经是0。
            }

            ++r; ++c;
        }
        return result;
    }
};